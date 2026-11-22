#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kdev_t.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mutex.h>

#define BUFF_SIZE 1024

unsigned char KBUF[BUFF_SIZE]="Initialized text";
static DEFINE_MUTEX(my_mutex);
dev_t my_dev = 0;


static int my_chr_open(struct inode *inode, struct file *fp){
	pr_info("My char file is opened!\n");
	return 0;
}

static int my_chr_release(struct inode *inode, struct file *fp){
	pr_info("Ny char file is released!\n");
	return 0;
}

static ssize_t my_chr_read(struct file *fp, char __user *buffer, size_t count, loff_t *len){
	pr_info("My chr read function is called!\n");
	mutex_lock(&my_mutex);
	size_t msg_len = min(count, (size_t)(BUFF_SIZE -1));
	if(*len >= msg_len)
	{
		mutex_unlock(&my_mutex);
		return 0;
	}
	if(copy_to_user(buffer,KBUF,msg_len)){
		mutex_unlock(&my_mutex);
		return -EFAULT;
	}
	*len += msg_len;
	mutex_unlock(&my_mutex);
	return msg_len;
}

static ssize_t my_chr_write(struct file *fp, const char __user *buffer, size_t count , loff_t *len){
	pr_info("My chr write function is called!\n");
	mutex_lock(&my_mutex);
	size_t msg_len = min(count, (size_t)(BUFF_SIZE-1));
	if(copy_from_user(KBUF,buffer,msg_len))
	{
		mutex_unlock(&my_mutex);
		return -EFAULT;
	}
	KBUF[msg_len]='\0';
	*len += msg_len;
	mutex_unlock(&my_mutex);
	return msg_len;
}

const struct file_operations my_fps = {

	.owner = THIS_MODULE,
	.read = my_chr_read,
	.write = my_chr_write,
	.open = my_chr_open,
	.release = my_chr_release,
};

static struct cdev my_cdev;
static struct class *my_class;



static int __init my_module_init(void)
{
	pr_info("My char driver module is called\n");
	
	if((alloc_chrdev_region(&my_dev, 0, 1, "my_char"))<0){
		pr_err("Not able to alloc Major\n");
		return -1;
	}
	pr_info("The Majon: %d & The Minor: %d\n",MAJOR(my_dev),MINOR(my_dev));
	cdev_init(&my_cdev, &my_fps);
	my_cdev.owner = THIS_MODULE;
	
	if(( cdev_add(&my_cdev, my_dev,1) )<0){
		pr_err("Not able to add the cdev to the system\n");
		return -1;
	}
	my_class = class_create("my_char");
	if(my_class == NULL){
		pr_err("Can not create the class struct\n");
		return -1;
	}
	if((device_create(my_class, NULL, my_dev,NULL,"my_char")) == NULL){
			  pr_err("Not able to create the char device 1\n");
			  return -1;
		  }
	

	return 0;
	
}


static void __exit my_module_exit(void)
{
	pr_info("My char driver module is exited\n");
	device_destroy(my_class, my_dev);
	class_destroy(my_class);
	cdev_del(&my_cdev);
	unregister_chrdev_region(my_dev,1);
	
	
}


module_init(my_module_init);
module_exit(my_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mahendra Reddy");
MODULE_DESCRIPTION("This is a character driver");
