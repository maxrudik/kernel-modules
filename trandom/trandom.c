#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/semaphore.h>
#include <linux/uaccess.h>
#include <linux/circ_buf.h>
#include <linux/miscdevice.h>
#include <linux/signal.h>
#include <linux/interrupt.h>

#define TRANDOM_DEV_NAME "trandom"
#define IRQ_NO 12
		
struct trandom_dev {
	char random_byte;
	bool is_empty;
	wait_queue_head_t read_q;
	struct semaphore sem;
};

struct trandom_dev *trandom_device;

ssize_t trandom_read(struct file *flip, char __user *buf, size_t count,
				loff_t *f_pos)
{
	ssize_t copied;
	if (down_interruptible(&trandom_device->sem))
		return -ERESTARTSYS;

	while (trandom_device->is_empty) {
		up(&trandom_device->sem);
		
		printk(KERN_INFO "%s: reading going to sleep\n", TRANDOM_DEV_NAME);
		
		if (wait_event_interruptible(trandom_device->read_q, 
					(!trandom_device->is_empty)))
			return -ERESTARTSYS;
		if (down_interruptible(&trandom_device->sem))
			return -ERESTARTSYS;	
	}	

	copied = 1 - copy_to_user(buf, &trandom_device->random_byte, 1);

	up(&trandom_device->sem);
	printk(KERN_INFO "%s: byte copied\n", TRANDOM_DEV_NAME);
	return copied;
}

ssize_t trandom_write(struct file *flip, const char __user *buf, size_t count,
				loff_t *f_pos)
{
	printk(KERN_INFO "%s: device write called\n", TRANDOM_DEV_NAME);
	return 0;
}

int trandom_open(struct inode *inode, struct file *flip)
{
	printk(KERN_INFO "%s: device is opened\n", TRANDOM_DEV_NAME);
	return 0;
}

int trandom_release(struct inode *inode, struct file *flip)
{
	printk(KERN_INFO "%s: device is closed\n", TRANDOM_DEV_NAME);

	return 0;
}

struct file_operations trandom_fops = {		
	.owner = THIS_MODULE,			
	.read = trandom_read,
	.write = trandom_write,
	.open = trandom_open,
	.release = trandom_release
};

static struct miscdevice trandom_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = TRANDOM_DEV_NAME,
	.nodename = TRANDOM_DEV_NAME,
	.fops = &trandom_fops,
	.mode = S_IRUGO
};

static irqreturn_t irq_handler(int irq, void *dev_id) {
  printk(KERN_INFO "%s: interrupt handled\n", TRANDOM_DEV_NAME);
  return IRQ_HANDLED;
}

void trandom_cleanup_module(void)
{
	kfree(trandom_device);

	misc_deregister(&trandom_miscdev);
	printk(KERN_INFO "%s: unregistered\n", TRANDOM_DEV_NAME);
	printk(KERN_INFO "%s: deleted /dev/%s\n", TRANDOM_DEV_NAME, TRANDOM_DEV_NAME);
}

static int trandom_init_module(void)
{

	int rv;
	trandom_device = kmalloc(sizeof(struct trandom_dev), GFP_KERNEL);

	if (!trandom_device) {
		printk(KERN_INFO "%s: kmalloc failed\n", TRANDOM_DEV_NAME);
		rv = -ENOMEM;
		goto fail;
	}

	memset(trandom_device, 0, sizeof(struct trandom_dev));
	
	trandom_device->random_byte = 0xff;
	trandom_device->is_empty = false;
	
	if (!trandom_device) {
		printk(KERN_INFO "%s: memset failed\n", TRANDOM_DEV_NAME);
		rv = -ENOMEM;
		goto fail;
	}

	sema_init(&trandom_device->sem, 1);
	init_waitqueue_head(&trandom_device->read_q);

	rv = misc_register(&trandom_miscdev);
	if (rv) {
		printk(KERN_INFO "%s: misc_register failed\n", TRANDOM_DEV_NAME);
		goto fail;
	}

	printk(KERN_INFO "%s: initialized\n", TRANDOM_DEV_NAME);
	printk(KERN_INFO "%s: created /dev/%s\n", TRANDOM_DEV_NAME, TRANDOM_DEV_NAME);

	//request_irq (1, (irq_handler_t) irq_handler, IRQF_SHARED, "kbd_irq_handler", (void *)(irq_handler));
	if (request_irq(IRQ_NO, irq_handler, IRQF_SHARED, "trandom", (void *)(irq_handler))) {
		printk(KERN_INFO "my_device: cannot register IRQ ");
			goto irq;
	}

	return 0;

irq: 
	free_irq(IRQ_NO,(void *)(irq_handler));

fail:
	printk(KERN_INFO "%s: initialization failed\n", TRANDOM_DEV_NAME);
	trandom_cleanup_module();
	return rv;
}

MODULE_AUTHOR("maxrudik");
MODULE_LICENSE("GPL");

module_init(trandom_init_module);
module_exit(trandom_cleanup_module);
