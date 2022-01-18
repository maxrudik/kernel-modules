#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/semaphore.h>
#include <linux/uaccess.h>
#include <linux/circ_buf.h>
#include <linux/signal.h>

#define BUFSIZE 16 
		
int nopipe_minor = 0;
int nopipe_major = 0;	

struct nopipe_dev {
	char *buffer;
	char *read_from, *write_to;
	int head, tail;
	int buf_size;
	wait_queue_head_t read_q, write_q;
	struct semaphore sem;
	struct cdev cdev;	
};

struct nopipe_dev *nopipe_device;

void print_buf(void)
{
	printk(KERN_INFO "buffer: %.16s\n", nopipe_device->buffer);
}

ssize_t nopipe_read(struct file *flip, char __user *buf, size_t count,
				loff_t *f_pos)
{
	struct nopipe_dev *dev = flip->private_data;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	while (!CIRC_CNT(dev->head, dev->tail, dev->buf_size)) {
		up(&dev->sem);
		
		printk(KERN_INFO "%s reading: going to sleep\n", current->comm);
		
		if (wait_event_interruptible(dev->read_q, 
					(CIRC_CNT(dev->head, dev->tail, dev->buf_size))))
			return -ERESTARTSYS;
		if (down_interruptible(&dev->sem))
			return -ERESTARTSYS;	
	}	

	count = min(count, CIRC_CNT(dev->head, dev->tail, dev->buf_size));
	int copied = count;

	int cont_part = min(count, CIRC_CNT_TO_END(dev->head, dev->tail, dev->buf_size));
	int rem_part = 0;
	if (count > CIRC_CNT_TO_END(dev->head, dev->tail, dev->buf_size))
		rem_part = count - cont_part;

	copied -= copy_to_user(buf, dev->buffer + dev->tail, cont_part);
	if (rem_part) {
		copied -= copy_to_user(buf + cont_part, dev->buffer, rem_part);
	}	
	dev->tail += copied;
	if (dev->tail >= dev->buf_size) {
		dev->tail -= dev->buf_size;
	}


	up(&dev->sem);
	wake_up(&dev->write_q);
	printk(KERN_INFO "nopipe: %d bytes copied", copied);
	print_buf();
	return copied;
}

ssize_t nopipe_write(struct file *flip, const char __user *buf, size_t count,
				loff_t *f_pos)
{
	struct nopipe_dev *dev = flip->private_data;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	while (!CIRC_SPACE(dev->head, dev->tail, dev->buf_size)) {
		up(&dev->sem);
		
		printk(KERN_INFO "%s writing: going to sleep\n", current->comm);
		
		if (wait_event_interruptible(dev->write_q, 
					(CIRC_SPACE(dev->head, dev->tail, dev->buf_size))))
			return -ERESTARTSYS;
		if (down_interruptible(&dev->sem))
			return -ERESTARTSYS;	
	}	

	count = min(count, CIRC_SPACE(dev->head, dev->tail, dev->buf_size));
	int copied = count;

	int cont_part = min(count, CIRC_SPACE_TO_END(dev->head, dev->tail, dev->buf_size));
	int rem_part = 0;
	if (count > CIRC_SPACE_TO_END(dev->head, dev->tail, dev->buf_size))
		rem_part = count - cont_part;

	copied -= copy_from_user(dev->buffer + dev->head, buf, cont_part);
	if (rem_part) {
		copied -= copy_from_user(dev->buffer, buf + cont_part, rem_part);
	}	
	dev->head += copied;
	if (dev->head >= dev->buf_size) {
		dev->head -= dev->buf_size;
	}

	up(&dev->sem);
	wake_up(&dev->read_q);
	printk(KERN_INFO "nopipe: %d bytes copied", copied);

	print_buf();

	return copied;
}

int nopipe_open(struct inode *inode, struct file *flip)
{
	struct nopipe_dev *dev;
	dev = container_of(inode->i_cdev, struct nopipe_dev, cdev);
	flip->private_data = dev;

	printk(KERN_INFO "nopipe: device is opened\n");

	return 0;
}

int nopipe_release(struct inode *inode, struct file *flip)
{
	printk(KERN_INFO "nopipe: device is closed\n");

	return 0;
}

struct file_operations nopipe_fops = {		
	.owner = THIS_MODULE,			
	.read = nopipe_read,
	.write = nopipe_write,
	.open = nopipe_open,
	.release = nopipe_release,
};

static void nopipe_setup_cdev(struct nopipe_dev *dev)
{
	int err, devno = MKDEV(nopipe_major, nopipe_minor);

	cdev_init(&dev->cdev, &nopipe_fops);

	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &nopipe_fops;

	err = cdev_add(&dev->cdev, devno, 1);

	if (err)
		printk(KERN_NOTICE "Error %d adding nopipe", err);
}

void nopipe_cleanup_module(void)
{
	dev_t devno = MKDEV(nopipe_major, nopipe_minor);

	cdev_del(&nopipe_device->cdev);
	kfree(nopipe_device->buffer);
	kfree(nopipe_device);

	unregister_chrdev_region(devno, 1); 
}

static int nopipe_init_module(void)
{
	int rv;
	dev_t dev;

	rv = alloc_chrdev_region(&dev, nopipe_minor, 1, "nopipe");	

	if (rv) {
		printk(KERN_WARNING "nopipe: can't get major %d\n", nopipe_major);
		return rv;
	}

	nopipe_major = MAJOR(dev);

	nopipe_device = kmalloc(sizeof(struct nopipe_dev), GFP_KERNEL);

	if (!nopipe_device) {
		rv = -ENOMEM;
		goto fail;
	}

	memset(nopipe_device, 0, sizeof(struct nopipe_dev));
	
	nopipe_device->head = 0;
	nopipe_device->tail = 0;
	nopipe_device->buf_size = BUFSIZE;
	nopipe_device->buffer = kmalloc(nopipe_device->buf_size, GFP_KERNEL);
	nopipe_device->read_from = nopipe_device->buffer;
	nopipe_device->write_to = nopipe_device->buffer;
	
	if (!nopipe_device) {
		rv = -ENOMEM;
		goto fail;
	}

	memset(nopipe_device->buffer, 0, nopipe_device->buf_size);

	sema_init(&nopipe_device->sem, 1);
	nopipe_setup_cdev(nopipe_device);
	init_waitqueue_head(&nopipe_device->read_q);
	init_waitqueue_head(&nopipe_device->write_q);

	printk(KERN_INFO "nopipe: major = %d minor = %d\n", nopipe_major, nopipe_minor);

	return 0;

fail:
	nopipe_cleanup_module();
	return rv;
}

MODULE_AUTHOR("maxrudik");
MODULE_LICENSE("GPL");

module_init(nopipe_init_module);
module_exit(nopipe_cleanup_module);
