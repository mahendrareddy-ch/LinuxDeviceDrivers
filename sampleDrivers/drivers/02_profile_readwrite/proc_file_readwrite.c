/* This kernel module is to create a file in /proc file and to read it from the user space */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>

#define PROCFILE_NAME "1_mahi"
#define MAX_BUFFER_SIZE 1024

static char msg_knl[MAX_BUFFER_SIZE] = "String initialized in kernel space buffer\n";

ssize_t	my_proc_write(struct file *fp, const char __user *buff, size_t count, loff_t *len)
{
	printk(KERN_INFO "proc file write function is called\n");
	size_t msg_knl_len = min(count, (size_t)(MAX_BUFFER_SIZE - 1));
	if (copy_from_user(msg_knl, buff, msg_knl_len))
	{
		return -1;
	}
	msg_knl[msg_knl_len] = '\0';
	*len = msg_knl_len;
	return msg_knl_len;
}

ssize_t	my_proc_read(struct file *fp, char __user *buf, size_t count, loff_t *len)
{
	printk(KERN_INFO "proc file read function is called\n");
	//char msg[MAX_BUFFER_SIZE] = "This is a proc file created by kernel module\n";
	size_t msg_len = strlen(msg_knl);
	if(*len >= msg_len)
	{
		return 0;
	}
	copy_to_user(buf, msg_knl, msg_len);
	*len += msg_len;
	return msg_len;
}

const struct proc_ops my_proc_ops = {
	//.owner = THIS_MODULE,
	.proc_read = &my_proc_read,
	.proc_write = &my_proc_write,
};

struct proc_dir_entry *my_proc_file = NULL;
 

static int __init my_module_init(void)
{
	pr_info("Module Init is called!\n");
	my_proc_file = proc_create(PROCFILE_NAME, 0666, NULL, &my_proc_ops);
	if(my_proc_file == NULL)
	{
		pr_err("Error creating proc file!\n");
		return -1;
	}
	return 0;
}

static void __exit my_module_exit(void)
{
	pr_info("Module exit is called!\n");
	proc_remove(my_proc_file);
}

module_init(my_module_init);
module_exit(my_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MAHENDRA REDDY");
MODULE_DESCRIPTION("This module is to register a proc file and to read the file contents from the user space app");