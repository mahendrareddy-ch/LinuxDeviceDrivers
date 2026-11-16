/* This is my sample char driver module */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>

static int __init hello_init(void)
{
	printk("This is a sample init call \n");
	return 0;
}

static void __exit hello_exit(void)
{
	printk("Module Exit\n");
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MAHI");
MODULE_DESCRIPTION("SAMPLE DRIVER");

