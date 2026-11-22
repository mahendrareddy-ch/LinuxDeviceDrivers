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
	{ 0x7706e61d, "usb_register_driver" },
	{ 0xe607ec3b, "usb_free_coherent" },
	{ 0x9a653306, "usb_free_urb" },
	{ 0xdb760f52, "__kfifo_free" },
	{ 0x9818b5d8, "usb_put_dev" },
	{ 0x037a0cba, "kfree" },
	{ 0xa6257a2f, "complete" },
	{ 0x45696576, "_dev_err" },
	{ 0x1a673812, "usb_submit_urb" },
	{ 0x34db050b, "_raw_spin_lock_irqsave" },
	{ 0xf23fcb99, "__kfifo_in" },
	{ 0xd35cce70, "_raw_spin_unlock_irqrestore" },
	{ 0x203bb998, "usb_deregister" },
	{ 0xcb09f834, "usb_find_interface" },
	{ 0x4dfa8d4b, "mutex_lock" },
	{ 0xa65c6def, "alt_cb_patch_nops" },
	{ 0x3213f038, "mutex_unlock" },
	{ 0x0296695f, "refcount_warn_saturate" },
	{ 0xb8342c83, "__kmalloc_noprof" },
	{ 0xf02aa937, "wait_for_completion_interruptible_timeout" },
	{ 0x13d0adf7, "__kfifo_out" },
	{ 0x6cbbfc54, "__arch_copy_to_user" },
	{ 0xb68ccd6f, "usb_deregister_dev" },
	{ 0xdcb7b9f6, "usb_kill_urb" },
	{ 0x93d6dd8c, "complete_all" },
	{ 0x40b8f921, "_dev_info" },
	{ 0x12a4e128, "__arch_copy_from_user" },
	{ 0xdcb764ad, "memset" },
	{ 0x4706e464, "kmalloc_caches" },
	{ 0x950c3596, "__kmalloc_cache_noprof" },
	{ 0xcefb0c9f, "__mutex_init" },
	{ 0x608741b5, "__init_swait_queue_head" },
	{ 0x139f2189, "__kfifo_alloc" },
	{ 0x76fccd34, "usb_get_dev" },
	{ 0xc4966978, "usb_alloc_urb" },
	{ 0x6d81f72e, "usb_alloc_coherent" },
	{ 0x2c9621df, "usb_register_dev" },
	{ 0x38022ea8, "module_layout" },
};

MODULE_INFO(depends, "");

MODULE_ALIAS("usb:v1234p5678d*dc*dsc*dp*ic*isc*ip*in*");

MODULE_INFO(srcversion, "8700549C04BD481EEED0B1B");
