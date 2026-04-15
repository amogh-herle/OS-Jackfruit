/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Single binary used two ways:
 *   Supervisor: ./engine supervisor <base-rootfs>
 *   CLI client: ./engine start|run|ps|logs|stop ...
 *
 * IPC design:
 *   Path A (logging): pipe per container -> pipe_reader_thread -> bounded_buffer -> logging_thread -> log file
 *   Path B (control): UNIX domain socket at /tmp/mini_runtime.sock (CLI <-> supervisor)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */
#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 512
#define CHILD_COMMAND_LEN   512
#define MAX_ARGV            16
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT  (40UL << 20)   /* 40 MiB */
#define DEFAULT_HARD_LIMIT  (64UL << 20)   /* 64 MiB */

/* ------------------------------------------------------------------ */
/*  Enums                                                               */
/* ------------------------------------------------------------------ */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED,
    CONTAINER_HARD_KILLED
} container_state_t;

/* ------------------------------------------------------------------ */
/*  Data structures                                                     */
/* ------------------------------------------------------------------ */
typedef struct container_record {
    char                   id[CONTAINER_ID_LEN];
    pid_t                  host_pid;
    time_t                 started_at;
    container_state_t      state;
    unsigned long          soft_limit_bytes;
    unsigned long          hard_limit_bytes;
    int                    exit_code;
    int                    exit_signal;
    int                    stop_requested;
    char                   log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char  id[CONTAINER_ID_LEN];
    char  rootfs[PATH_MAX];
    char  command[CHILD_COMMAND_LEN];
    int   nice_value;
    int   log_write_fd;
} child_config_t;

typedef struct {
    int                 server_fd;
    int                 monitor_fd;
    int                 should_stop;
    pthread_t           logger_thread;
    bounded_buffer_t    log_buffer;
    pthread_mutex_t     metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* pipe_reader thread args */
typedef struct {
    int   read_fd;
    char  container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} pipe_reader_args_t;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                                */
/* ------------------------------------------------------------------ */
static int  run_supervisor(const char *rootfs);
static int  send_control_request(const control_request_t *req);
static int  cmd_start(int argc, char *argv[]);
static int  cmd_run(int argc, char *argv[]);
static int  cmd_ps(void);
static int  cmd_logs(int argc, char *argv[]);
static int  cmd_stop(int argc, char *argv[]);
static pid_t launch_container(supervisor_ctx_t *ctx, const control_request_t *req);
static void  handle_control_request(supervisor_ctx_t *ctx, int client_fd);
static void *logging_thread(void *arg);
static void *pipe_reader_thread(void *arg);
static int   child_fn(void *arg);
static int   register_with_monitor(int monitor_fd, const char *container_id,
                                   pid_t host_pid, unsigned long soft, unsigned long hard);
static int   unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid);

/* Global supervisor context pointer for signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs  <id>\n"
            "  %s stop  <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag, const char *value, unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr, "Invalid value for --nice (expected -20..19): %s\n", argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:    return "starting";
    case CONTAINER_RUNNING:     return "running";
    case CONTAINER_STOPPED:     return "stopped";
    case CONTAINER_KILLED:      return "killed";
    case CONTAINER_EXITED:      return "exited";
    case CONTAINER_HARD_KILLED: return "hard_limit_killed";
    default:                    return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/*  Bounded buffer (producer-consumer, mutex + condvar)                 */
/* ------------------------------------------------------------------ */

/*
 * Why mutex + condvars?
 * - The producer (pipe_reader_thread) and consumer (logging_thread) run
 *   concurrently. Without synchronization, concurrent reads/writes to head,
 *   tail, and count are data races that cause lost data or corruption.
 * - A mutex protects the shared state; condvars let threads sleep instead
 *   of spinning, avoiding busy-wait CPU waste.
 * - Two condvars (not_empty, not_full) allow producers and consumers to
 *   wake each other independently, preventing deadlock.
 */
static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * Producer: insert a log chunk into the buffer.
 * Blocks when the buffer is full; returns -1 on shutdown.
 *
 * Race condition without this lock:
 *   Two producers could both read count < CAPACITY, both write to the
 *   same tail slot, and one item would be silently overwritten.
 */
static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Block while full, bail on shutdown */
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);   /* wake consumer */
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Consumer: remove a log chunk from the buffer.
 * Blocks when empty; returns -1 when shutdown and buffer drained.
 *
 * Race condition without this lock:
 *   The consumer could read an item while a producer is mid-write,
 *   producing a torn read with partial new + partial old data.
 */
static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait while empty; on shutdown drain remaining items first */
    while (buffer->count == 0) {
        if (buffer->shutting_down) {
            pthread_mutex_unlock(&buffer->mutex);
            return -1;   /* tell consumer to exit */
        }
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);   /* wake producer */
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Logging consumer thread                                             */
/* ------------------------------------------------------------------ */

/*
 * Runs continuously; pops chunks from the shared buffer and appends them
 * to per-container log files.  Exits when bounded_buffer_pop returns -1
 * (shutdown + drained).
 */
static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        /*
         * Look up the container log path under the metadata lock to avoid
         * a race with record removal.
         */
        char log_path[PATH_MAX];
        int  found = 0;

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = ctx->containers;
        while (rec) {
            if (strncmp(rec->id, item.container_id, CONTAINER_ID_LEN) == 0) {
                strncpy(log_path, rec->log_path, sizeof(log_path) - 1);
                log_path[sizeof(log_path) - 1] = '\0';
                found = 1;
                break;
            }
            rec = rec->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (!found)
            snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Pipe reader thread (one per container, producer side)               */
/* ------------------------------------------------------------------ */

/*
 * Reads stdout+stderr from the container's pipe and pushes chunks into
 * the shared bounded buffer.  Exits when the pipe read end is closed
 * (container exited).
 */
static void *pipe_reader_thread(void *arg)
{
    pipe_reader_args_t *pra = (pipe_reader_args_t *)arg;
    log_item_t item;
    ssize_t n;

    while (1) {
        n = read(pra->read_fd, item.data, sizeof(item.data));
        if (n <= 0)
            break;
        item.length = (size_t)n;
        strncpy(item.container_id, pra->container_id, CONTAINER_ID_LEN - 1);
        item.container_id[CONTAINER_ID_LEN - 1] = '\0';
        bounded_buffer_push(pra->log_buffer, &item);
    }

    close(pra->read_fd);
    free(pra);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Clone child entrypoint                                              */
/* ------------------------------------------------------------------ */

/*
 * Runs inside the new PID/UTS/mount namespace created by clone().
 * Responsibilities:
 *   1. Apply nice value (scheduling priority).
 *   2. Redirect stdout/stderr into the supervisor-side pipe (logging path A).
 *   3. chroot into the container's private rootfs copy.
 *   4. Mount /proc so in-container tools work.
 *   5. Set a distinct UTS hostname.
 *   6. execvp the requested command.
 */
static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* 1. Nice value */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* 2. Redirect stdout/stderr -> pipe write end */
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    /* 3. chroot into the container-private rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }

    /* 4. Mount /proc (non-fatal if it fails) */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0)
        perror("mount /proc (non-fatal)");

    /* 5. Set hostname = container id (UTS namespace isolation) */
    sethostname(cfg->id, strlen(cfg->id));

    /* 6. Split command string into argv and exec */
    char  cmd_copy[CHILD_COMMAND_LEN];
    strncpy(cmd_copy, cfg->command, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';

    char *argv_arr[MAX_ARGV + 1];
    int   argc_arr = 0;
    char *tok = strtok(cmd_copy, " \t");
    while (tok && argc_arr < MAX_ARGV) {
        argv_arr[argc_arr++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv_arr[argc_arr] = NULL;

    if (argc_arr == 0) {
        fprintf(stderr, "child_fn: empty command\n");
        return 1;
    }

    execvp(argv_arr[0], argv_arr);
    perror("execvp");
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Kernel monitor ioctl helpers                                        */
/* ------------------------------------------------------------------ */
static int register_with_monitor(int monitor_fd, const char *container_id,
                                 pid_t host_pid, unsigned long soft_limit_bytes,
                                 unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

static int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Signal handlers                                                     */
/* ------------------------------------------------------------------ */

/*
 * SIGCHLD: reap every exited child with WNOHANG in a loop, update
 * container metadata, unregister from kernel monitor.
 *
 * Using SA_RESTART so that accept() is not permanently interrupted.
 * errno is saved/restored because signal handlers can clobber it.
 */
static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    int wstatus;
    pid_t pid;

    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        if (!g_ctx)
            continue;

        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *rec = g_ctx->containers;
        while (rec) {
            if (rec->host_pid == pid) {
                if (WIFEXITED(wstatus)) {
                    rec->state     = CONTAINER_EXITED;
                    rec->exit_code = WEXITSTATUS(wstatus);
                } else if (WIFSIGNALED(wstatus)) {
                    rec->exit_signal = WTERMSIG(wstatus);
                    rec->exit_code   = 128 + rec->exit_signal;
                    if (rec->stop_requested)
                        rec->state = CONTAINER_STOPPED;
                    else if (rec->exit_signal == SIGKILL)
                        rec->state = CONTAINER_HARD_KILLED;
                    else
                        rec->state = CONTAINER_KILLED;
                }
                break;
            }
            rec = rec->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);

        /* Unregister from kernel monitor */
        if (g_ctx->monitor_fd >= 0 && rec)
            unregister_from_monitor(g_ctx->monitor_fd, rec->id, pid);
    }

    errno = saved_errno;
}

/*
 * SIGINT / SIGTERM: signal the supervisor event loop to stop.
 * The loop checks should_stop after each accept() returns.
 */
static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

/* ------------------------------------------------------------------ */
/*  Container launch                                                    */
/* ------------------------------------------------------------------ */

/*
 * Allocates a pipe, clones the child into new namespaces, starts a
 * pipe_reader_thread, registers with the kernel monitor, and inserts a
 * metadata record.  Returns the new host PID on success, -1 on failure.
 */
static pid_t launch_container(supervisor_ctx_t *ctx, const control_request_t *req)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return -1;
    }

    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    strncpy(cfg->id,      req->container_id, sizeof(cfg->id) - 1);
    strncpy(cfg->rootfs,  req->rootfs,       sizeof(cfg->rootfs) - 1);
    strncpy(cfg->command, req->command,      sizeof(cfg->command) - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefd[1];   /* child writes here */

    /* Stack for the cloned child */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    /*
     * CLONE_NEWPID  - new PID namespace (container sees itself as PID 1)
     * CLONE_NEWUTS  - new UTS namespace (independent hostname)
     * CLONE_NEWNS   - new mount namespace (independent /proc mount)
     * SIGCHLD       - notify parent when child exits
     */
    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, clone_flags, cfg);
    if (pid < 0) {
        perror("clone");
        free(stack);
        free(cfg);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    /*
     * The child runs on its own copy of the stack pages (COW after clone).
     * The parent's mapping is no longer needed and must be freed to avoid
     * a STACK_SIZE (1 MiB) leak per launched container.
     */
    free(stack);

    /* Parent closes the write end; only the child writes */
    close(pipefd[1]);

    /* Start a pipe reader thread to push container output into the buffer */
    pipe_reader_args_t *pra = malloc(sizeof(*pra));
    if (pra) {
        pra->read_fd = pipefd[0];
        strncpy(pra->container_id, req->container_id, sizeof(pra->container_id) - 1);
        pra->container_id[sizeof(pra->container_id) - 1] = '\0';
        pra->log_buffer = &ctx->log_buffer;

        pthread_t tid;
        if (pthread_create(&tid, NULL, pipe_reader_thread, pra) != 0) {
            free(pra);
            close(pipefd[0]);
        } else {
            pthread_detach(tid);   /* auto-cleanup when it exits */
        }
    } else {
        close(pipefd[0]);
    }

    /* Register with kernel memory monitor */
    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);

    /* Insert metadata record at head of list (lock-protected) */
    container_record_t *rec = calloc(1, sizeof(*rec));
    if (rec) {
        strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
        rec->host_pid         = pid;
        rec->started_at       = time(NULL);
        rec->state            = CONTAINER_RUNNING;
        rec->soft_limit_bytes = req->soft_limit_bytes;
        rec->hard_limit_bytes = req->hard_limit_bytes;
        snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log",
                 LOG_DIR, req->container_id);

        pthread_mutex_lock(&ctx->metadata_lock);
        rec->next       = ctx->containers;
        ctx->containers = rec;
        pthread_mutex_unlock(&ctx->metadata_lock);
    }

    return pid;
}

/* ------------------------------------------------------------------ */
/*  Control request handler (supervisor side)                           */
/* ------------------------------------------------------------------ */
static void handle_control_request(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t  req;
    control_response_t resp;
    ssize_t n;

    memset(&resp, 0, sizeof(resp));

    n = read(client_fd, &req, sizeof(req));
    if (n != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Bad request size");
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    switch (req.kind) {

    case CMD_START: {
        pid_t pid = launch_container(ctx, &req);
        if (pid < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Failed to start container %s", req.container_id);
        } else {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "Container %s started (pid %d)", req.container_id, pid);
        }
        break;
    }

    case CMD_RUN: {
        /*
         * Foreground run: start container, wait for it here in the
         * supervisor, then report the exit status to the client.
         */
        pid_t pid = launch_container(ctx, &req);
        if (pid < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Failed to start container %s", req.container_id);
            break;
        }

        int wstatus = 0;
        waitpid(pid, &wstatus, 0);

        int exit_code = WIFEXITED(wstatus)  ? WEXITSTATUS(wstatus) :
                        WIFSIGNALED(wstatus) ? 128 + WTERMSIG(wstatus) : -1;

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = ctx->containers;
        while (rec) {
            if (rec->host_pid == pid) {
                rec->exit_code = exit_code;
                if (WIFEXITED(wstatus))
                    rec->state = CONTAINER_EXITED;
                else if (WIFSIGNALED(wstatus)) {
                    int sig = WTERMSIG(wstatus);
                    if (rec->stop_requested)
                        rec->state = CONTAINER_STOPPED;
                    else if (sig == SIGKILL)
                        rec->state = CONTAINER_HARD_KILLED;
                    else
                        rec->state = CONTAINER_KILLED;
                }
                break;
            }
            rec = rec->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = exit_code;
        snprintf(resp.message, sizeof(resp.message),
                 "Container %s exited (code %d)", req.container_id, exit_code);
        break;
    }

    case CMD_PS: {
        char *buf = resp.message;
        int   rem = sizeof(resp.message);
        int   off = 0;

        off += snprintf(buf + off, rem - off,
                        "%-16s %-8s %-18s %-10s %-10s\n",
                        "ID", "PID", "STATE", "SOFT(MiB)", "HARD(MiB)");

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = ctx->containers;
        while (rec && off < rem - 1) {
            char started[32];
            struct tm *tm_info = localtime(&rec->started_at);
            strftime(started, sizeof(started), "%H:%M:%S", tm_info);

            off += snprintf(buf + off, rem - off,
                            "%-16s %-8d %-18s %-10lu %-10lu  started=%s\n",
                            rec->id, rec->host_pid,
                            state_to_string(rec->state),
                            rec->soft_limit_bytes >> 20,
                            rec->hard_limit_bytes >> 20,
                            started);
            rec = rec->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = 0;
        break;
    }

    case CMD_LOGS: {
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req.container_id);

        int fd = open(log_path, O_RDONLY);
        if (fd < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "No log found for container %s", req.container_id);
            write(client_fd, &resp, sizeof(resp));
            return;
        }

        /*
         * Stream the log in CONTROL_MESSAGE_LEN-1 byte chunks so that large
         * log files are not silently truncated to sizeof(resp.message)-1 bytes.
         * Each chunk is sent as a separate response (status=0).
         * A final EOF sentinel response (status=1, empty message) signals the
         * client that all data has been delivered.
         */
        {
            ssize_t r;
            while ((r = read(fd, resp.message, sizeof(resp.message) - 1)) > 0) {
                resp.message[r] = '\0';
                resp.status = 0;
                write(client_fd, &resp, sizeof(resp));
                memset(&resp, 0, sizeof(resp));
            }
            close(fd);
        }

        /* EOF sentinel */
        memset(&resp, 0, sizeof(resp));
        resp.status = 1;
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    case CMD_STOP: {
        pid_t target_pid = -1;

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = ctx->containers;
        while (rec) {
            if (strncmp(rec->id, req.container_id, CONTAINER_ID_LEN) == 0) {
                if (rec->state == CONTAINER_RUNNING ||
                    rec->state == CONTAINER_STARTING) {
                    target_pid          = rec->host_pid;
                    rec->stop_requested = 1;
                }
                break;
            }
            rec = rec->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (target_pid < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "No running container with id '%s'", req.container_id);
        } else {
            kill(target_pid, SIGTERM);
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "Sent SIGTERM to container %s (pid %d)",
                     req.container_id, target_pid);
        }
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Unknown command");
        break;
    case CMD_SUPERVISOR:
        /* CMD_SUPERVISOR is only valid as a process mode, not a control request */
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Invalid command over socket");
        break;
    }

    write(client_fd, &resp, sizeof(resp));
}

/* ------------------------------------------------------------------ */
/*  Supervisor main                                                     */
/* ------------------------------------------------------------------ */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;

    /* Initialize metadata lock */
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    /* Initialize bounded log buffer */
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    printf("Supervisor started with base rootfs: %s\n", rootfs);

    /* 1) Create logs directory */
    mkdir(LOG_DIR, 0755);

    /* 2) Open kernel monitor device (non-fatal if module not loaded) */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: cannot open /dev/container_monitor (%s) - monitoring disabled\n",
                strerror(errno));

    /* 3) Create and bind the UNIX domain control socket (Path B - IPC) */
    unlink(CONTROL_PATH);

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(ctx.server_fd);
        return 1;
    }

    chmod(CONTROL_PATH, 0666);

    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen");
        close(ctx.server_fd);
        return 1;
    }

    /* 4) Install signal handlers */
    g_ctx = &ctx;

    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa_chld.sa_mask);
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_handler;
    sa_term.sa_flags   = SA_RESTART;
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGINT,  &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);

    /* 5) Start the logging consumer thread */
    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create logger");
        close(ctx.server_fd);
        return 1;
    }

    /* 6) Supervisor event loop: accept and handle CLI connections */
    printf("Supervisor listening on %s\n", CONTROL_PATH);

    while (!ctx.should_stop) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;   /* interrupted by SIGCHLD or SIGTERM, loop */
            perror("accept");
            break;
        }
        handle_control_request(&ctx, client_fd);
        close(client_fd);
    }

    /* ---- Orderly shutdown ---- */
    printf("Supervisor shutting down...\n");

    /* SIGTERM all still-running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *rec = ctx.containers;
    while (rec) {
        if (rec->state == CONTAINER_RUNNING || rec->state == CONTAINER_STARTING)
            kill(rec->host_pid, SIGTERM);
        rec = rec->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Give containers a moment then reap stragglers */
    sleep(1);
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

    /* Drain and stop the log pipeline */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* Free all container metadata records */
    pthread_mutex_lock(&ctx.metadata_lock);
    rec = ctx.containers;
    while (rec) {
        container_record_t *next = rec->next;
        free(rec);
        rec = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Close fds and clean up socket file */
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    unlink(CONTROL_PATH);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    printf("Supervisor exited cleanly.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  CLI client: send a control request to the running supervisor        */
/* ------------------------------------------------------------------ */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write");
        close(fd);
        return 1;
    }

    control_response_t resp;
    ssize_t n = read(fd, &resp, sizeof(resp));
    close(fd);

    if (n != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "Truncated response from supervisor\n");
        return 1;
    }

    printf("%s\n", resp.message);
    return resp.status == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/*  CLI command implementations                                         */
/* ------------------------------------------------------------------ */
static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    {
        int cmd_end = 5;
        while (cmd_end < argc && argv[cmd_end][0] != '-')
            cmd_end++;

        strncpy(req.command, argv[4], sizeof(req.command) - 1);
        int i;
        for (i = 5; i < cmd_end; i++) {
            strncat(req.command, " ",    sizeof(req.command) - strlen(req.command) - 1);
            strncat(req.command, argv[i], sizeof(req.command) - strlen(req.command) - 1);
        }
        if (parse_optional_flags(&req, argc, argv, cmd_end) != 0)
            return 1;
    }

    printf("Starting container: %s\n", req.container_id);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/*  Signal forwarding for cmd_run                                       */
/* ------------------------------------------------------------------ */

/*
 * When the user presses Ctrl-C (SIGINT) or sends SIGTERM while `run` is
 * blocking on a response, we must forward a stop request to the supervisor
 * rather than simply dying.  g_run_container_id holds the container id that
 * is currently being waited on, and g_run_interrupted is set by the handler.
 */
static volatile sig_atomic_t g_run_interrupted    = 0;
static char                  g_run_container_id[CONTAINER_ID_LEN];

static void run_sigforward_handler(int sig)
{
    (void)sig;
    g_run_interrupted = 1;
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> "
                "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    {
        int cmd_end = 5;
        while (cmd_end < argc && argv[cmd_end][0] != '-')
            cmd_end++;

        strncpy(req.command, argv[4], sizeof(req.command) - 1);
        int i;
        for (i = 5; i < cmd_end; i++) {
            strncat(req.command, " ",    sizeof(req.command) - strlen(req.command) - 1);
            strncat(req.command, argv[i], sizeof(req.command) - strlen(req.command) - 1);
        }
        if (parse_optional_flags(&req, argc, argv, cmd_end) != 0)
            return 1;
    }

    /*
     * Save the container id for the signal handler, then install handlers
     * so that SIGINT/SIGTERM while we block on the supervisor's response
     * results in a proper `stop <id>` forwarded to the supervisor.
     */
    strncpy(g_run_container_id, req.container_id, sizeof(g_run_container_id) - 1);
    g_run_container_id[sizeof(g_run_container_id) - 1] = '\0';

    struct sigaction sa_fwd, sa_old_int, sa_old_term;
    memset(&sa_fwd, 0, sizeof(sa_fwd));
    sa_fwd.sa_handler = run_sigforward_handler;
    sigemptyset(&sa_fwd.sa_mask);
    /* Do NOT use SA_RESTART: we want read() to return EINTR so we notice */
    sigaction(SIGINT,  &sa_fwd, &sa_old_int);
    sigaction(SIGTERM, &sa_fwd, &sa_old_term);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(fd);
        sigaction(SIGINT,  &sa_old_int,  NULL);
        sigaction(SIGTERM, &sa_old_term, NULL);
        return 1;
    }

    if (write(fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        perror("write");
        close(fd);
        sigaction(SIGINT,  &sa_old_int,  NULL);
        sigaction(SIGTERM, &sa_old_term, NULL);
        return 1;
    }

    control_response_t resp;
    ssize_t n = read(fd, &resp, sizeof(resp));
    close(fd);

    /* Restore original signal dispositions */
    sigaction(SIGINT,  &sa_old_int,  NULL);
    sigaction(SIGTERM, &sa_old_term, NULL);

    if (g_run_interrupted) {
        /*
         * User hit Ctrl-C (or SIGTERM arrived) while waiting for the
         * container to finish.  Forward a stop request to the supervisor
         * so the container is terminated gracefully, then exit.
         */
        fprintf(stderr, "\nInterrupted — forwarding stop to container %s\n",
                g_run_container_id);
        control_request_t stop_req;
        memset(&stop_req, 0, sizeof(stop_req));
        stop_req.kind = CMD_STOP;
        strncpy(stop_req.container_id, g_run_container_id,
                sizeof(stop_req.container_id) - 1);
        send_control_request(&stop_req);
        return 130;  /* conventional exit code for Ctrl-C */
    }

    if (n != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "Truncated response from supervisor\n");
        return 1;
    }

    printf("%s\n", resp.message);
    return resp.status == 0 ? 0 : 1;
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(fd);
        return 1;
    }

    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    if (write(fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        perror("write");
        close(fd);
        return 1;
    }

    /*
     * The supervisor streams the log in chunks (status=0) followed by an
     * EOF sentinel (status=1).  Print each chunk as it arrives so large
     * logs are not truncated to a single 511-byte response.
     */
    control_response_t resp;
    int ret = 0;
    while (1) {
        ssize_t n = read(fd, &resp, sizeof(resp));
        if (n != (ssize_t)sizeof(resp)) {
            fprintf(stderr, "Truncated response from supervisor\n");
            ret = 1;
            break;
        }
        if (resp.status == 1)   /* EOF sentinel */
            break;
        if (resp.status < 0) {
            fprintf(stderr, "%s\n", resp.message);
            ret = 1;
            break;
        }
        printf("%s", resp.message);
        fflush(stdout);
    }

    close(fd);
    return ret;
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/*  main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}