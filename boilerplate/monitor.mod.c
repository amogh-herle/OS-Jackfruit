#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xefd5d5d8, "cdev_init" },
	{ 0x4c1dbbd9, "cdev_add" },
	{ 0x02f9bbf0, "init_timer_key" },
	{ 0x058c185a, "jiffies" },
	{ 0x32feeafc, "mod_timer" },
	{ 0xe8213e80, "_printk" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0x0bc5fb0d, "unregister_chrdev_region" },
	{ 0x6fdeeff0, "device_destroy" },
	{ 0x14fcde53, "class_destroy" },
	{ 0x092a35a2, "_copy_from_user" },
	{ 0xe1e1f979, "_raw_spin_lock_irqsave" },
	{ 0x2435d559, "strncmp" },
	{ 0x81a1a811, "_raw_spin_unlock_irqrestore" },
	{ 0xcb8b6ec6, "kfree" },
	{ 0xbd03ed67, "random_kmalloc_seed" },
	{ 0xfed1e3bc, "kmalloc_caches" },
	{ 0x70db3fe4, "__kmalloc_cache_noprof" },
	{ 0xc609ff70, "strncpy" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0x2352b148, "timer_delete_sync" },
	{ 0x0c72f9ad, "cdev_del" },
	{ 0x397daafe, "mmput" },
	{ 0xd272d446, "__rcu_read_lock" },
	{ 0xa009a8d7, "find_get_pid" },
	{ 0x774b89a0, "pid_task" },
	{ 0xbaf098df, "put_pid" },
	{ 0xd272d446, "__rcu_read_unlock" },
	{ 0xe8d8d116, "get_task_mm" },
	{ 0x7f6d5e1a, "__put_task_struct" },
	{ 0x2d95acaf, "send_sig" },
	{ 0x2520ea93, "refcount_warn_saturate" },
	{ 0x90a48d82, "__ubsan_handle_out_of_bounds" },
	{ 0xd272d446, "__fentry__" },
	{ 0x9f222e1e, "alloc_chrdev_region" },
	{ 0xea5ac1d9, "class_create" },
	{ 0xf98f93a7, "device_create" },
	{ 0xba157484, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0xefd5d5d8,
	0x4c1dbbd9,
	0x02f9bbf0,
	0x058c185a,
	0x32feeafc,
	0xe8213e80,
	0xd272d446,
	0x0bc5fb0d,
	0x6fdeeff0,
	0x14fcde53,
	0x092a35a2,
	0xe1e1f979,
	0x2435d559,
	0x81a1a811,
	0xcb8b6ec6,
	0xbd03ed67,
	0xfed1e3bc,
	0x70db3fe4,
	0xc609ff70,
	0xd272d446,
	0x2352b148,
	0x0c72f9ad,
	0x397daafe,
	0xd272d446,
	0xa009a8d7,
	0x774b89a0,
	0xbaf098df,
	0xd272d446,
	0xe8d8d116,
	0x7f6d5e1a,
	0x2d95acaf,
	0x2520ea93,
	0x90a48d82,
	0xd272d446,
	0x9f222e1e,
	0xea5ac1d9,
	0xf98f93a7,
	0xba157484,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"cdev_init\0"
	"cdev_add\0"
	"init_timer_key\0"
	"jiffies\0"
	"mod_timer\0"
	"_printk\0"
	"__x86_return_thunk\0"
	"unregister_chrdev_region\0"
	"device_destroy\0"
	"class_destroy\0"
	"_copy_from_user\0"
	"_raw_spin_lock_irqsave\0"
	"strncmp\0"
	"_raw_spin_unlock_irqrestore\0"
	"kfree\0"
	"random_kmalloc_seed\0"
	"kmalloc_caches\0"
	"__kmalloc_cache_noprof\0"
	"strncpy\0"
	"__stack_chk_fail\0"
	"timer_delete_sync\0"
	"cdev_del\0"
	"mmput\0"
	"__rcu_read_lock\0"
	"find_get_pid\0"
	"pid_task\0"
	"put_pid\0"
	"__rcu_read_unlock\0"
	"get_task_mm\0"
	"__put_task_struct\0"
	"send_sig\0"
	"refcount_warn_saturate\0"
	"__ubsan_handle_out_of_bounds\0"
	"__fentry__\0"
	"alloc_chrdev_region\0"
	"class_create\0"
	"device_create\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "51DFF0DCD2E71C3E48F321E");
