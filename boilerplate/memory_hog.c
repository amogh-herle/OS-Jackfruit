/*
 * memory_hog.c - Memory pressure workload for soft / hard limit testing.
 *
 * Allocates memory in small 1 MiB chunks so RSS grows gradually,
 * reliably crossing the soft limit before the hard limit.
 *
 * Usage:
 *   /memory_hog [chunk_mb] [sleep_ms]
 *
 * Recommended for soft/hard limit demo:
 *   /memory_hog 1 500   (1MB every 500ms)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static size_t parse_size_mb(const char *arg, size_t fallback)
{
    char *end = NULL;
    unsigned long value = strtoul(arg, &end, 10);
    if (!arg || *arg == '\0' || (end && *end != '\0') || value == 0)
        return fallback;
    return (size_t)value;
}

static useconds_t parse_sleep_ms(const char *arg, useconds_t fallback)
{
    char *end = NULL;
    unsigned long value = strtoul(arg, &end, 10);
    if (!arg || *arg == '\0' || (end && *end != '\0'))
        return fallback;
    return (useconds_t)(value * 1000U);
}

int main(int argc, char *argv[])
{
    const size_t chunk_mb    = (argc > 1) ? parse_size_mb(argv[1], 1) : 1;
    const useconds_t sleep_us = (argc > 2) ? parse_sleep_ms(argv[2], 500U) : 500U * 1000U;
    const size_t chunk_bytes  = chunk_mb * 1024UL * 1024UL;
    size_t total_mb = 0;
    int count = 0;

    /* Keep all allocations alive so RSS actually grows */
    void **allocs = NULL;
    size_t alloc_cap = 0;

    while (1) {
        /* grow allocs array if needed */
        if ((size_t)count >= alloc_cap) {
            alloc_cap = alloc_cap ? alloc_cap * 2 : 64;
            void **tmp = realloc(allocs, alloc_cap * sizeof(void *));
            if (!tmp) {
                printf("realloc failed after %d allocations\n", count);
                break;
            }
            allocs = tmp;
        }

        char *mem = malloc(chunk_bytes);
        if (!mem) {
            printf("malloc failed after %d allocations\n", count);
            break;
        }

        /* Touch every page so it actually maps into RSS */
        memset(mem, 'A', chunk_bytes);
        allocs[count] = mem;
        count++;
        total_mb += chunk_mb;

        printf("allocation=%d chunk=%zuMB total=%zuMB\n",
               count, chunk_mb, total_mb);
        fflush(stdout);
        usleep(sleep_us);
    }

    /* Not reached normally - killed by hard limit */
    for (int i = 0; i < count; i++)
        free(allocs[i]);
    free(allocs);
    return 0;
}