/* 458ramdisk.c
 * Andrew Holbrook
 *
 * Custom ramdisk kernel (2.6.32) module.
 */

#define SUCCESS		0
#define DEV_NAME "558ramdisk"
#define PROC_NAME "cs558ramdisk"

// The log can only hold the last "LOG_SIZE" I/O operations
#define LOG_SIZE 100

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/bio.h>
#include <linux/stat.h>
#include <linux/proc_fs.h>

// struct for logging read/writes
struct rw_info
{
	unsigned int type;
	unsigned int start_sector;
	unsigned int num_bytes;
	long unsigned int time;
};

MODULE_LICENSE("Dual BSD/GPL");

// prototypes
static int ramdisk_open(struct block_device *, fmode_t);
static int ramdisk_release(struct gendisk *, fmode_t);
static void ramdisk_request(struct request_queue *);
static int ramdisk_media_changed(struct gendisk *gd);
static int ramdisk_revalidate(struct gendisk *gd);
static int ramdisk_ioctl(struct block_device *, fmode_t, unsigned int,
						 unsigned long);
static int read_func(char *, char**, off_t, int, int *, void *);


// device- and ramdisk-related variables
static int _major = 0;
static struct gendisk *_gd;
static spinlock_t _lock;
static struct request_queue *_queue;
static u8 *_ramdisk_data;

// user-configurable variables
static unsigned long ramdisk_size = 512 * 2048;
module_param(ramdisk_size, long, 0);
static unsigned int debug = 0;
module_param(debug, int, 0);

// log-related variables
static struct rw_info rw_log[LOG_SIZE];
static unsigned int log_ptr = 0;
static unsigned int cur_log_size = 0;
static off_t _proc_off;
static spinlock_t _log_lock;

// procfs
static struct proc_dir_entry *pdir;

static struct block_device_operations bops = {
	.owner = THIS_MODULE,
	.open = ramdisk_open,
	.release = ramdisk_release,
	.media_changed   = ramdisk_media_changed,
	.revalidate_disk = ramdisk_revalidate,
	.ioctl	         = ramdisk_ioctl,
};

static int ramdisk_init(void)
{
	unsigned long nsectors;

	_major = register_blkdev(_major, DEV_NAME);
	if (_major <= 0) {
		return -1;
	}

	spin_lock_init(&_lock);

	_ramdisk_data = vmalloc(ramdisk_size);
	if (!_ramdisk_data) {
		printk(KERN_INFO "vmalloc failed.\n");
		unregister_blkdev(_major, DEV_NAME);
		return -1;
	}

	memset(_ramdisk_data, 0, ramdisk_size);

	_queue = blk_init_queue(ramdisk_request, &_lock);
	if (!_queue) {
		printk(KERN_INFO "queue creation failed.\n");
		vfree(_ramdisk_data);
		unregister_blkdev(_major, DEV_NAME);
		return -1;
	}

	_gd = alloc_disk(16);
	if (!_gd) {
		printk(KERN_INFO "gendisk creation failed.\n");
		blk_cleanup_queue(_queue);
		vfree(_ramdisk_data);
		unregister_blkdev(_major, DEV_NAME);
		return -1;
	}

	_gd->major = _major;
	_gd->first_minor = 0;
	_gd->fops = &bops;
	_gd->queue = _queue;
	strcpy(_gd->disk_name, "cs558ramdisk");

	nsectors = ramdisk_size / 512;
	if (ramdisk_size % 512)
		++nsectors;

	set_capacity(_gd, nsectors);
	add_disk(_gd);

	pdir = create_proc_entry(PROC_NAME, S_IRUSR | S_IRGRP | S_IROTH, NULL);
	pdir->read_proc = read_func;

	printk(KERN_INFO "558ramdisk loaded\n");
	return SUCCESS;
}

static void ramdisk_cleanup(void)
{
	if (_gd) {
		del_gendisk(_gd);
	}

	if (_queue) {
		blk_cleanup_queue(_queue);
	}

	if (_ramdisk_data) {
		vfree(_ramdisk_data);
	}

	unregister_blkdev(_major, DEV_NAME);

	remove_proc_entry(PROC_NAME, NULL);

	printk(KERN_INFO "558ramdisk unloaded\n");
}

/*
 *	Does nothing
 */
static int ramdisk_open(struct block_device *blk_dev, fmode_t f)
{
	return SUCCESS;
}

/*
 *	Does nothing
 */
static int ramdisk_release(struct gendisk *gd, fmode_t f)
{
	return SUCCESS;
}

/*
 *	This is where the OS will request I/O with the ramdisk.
 */
static void ramdisk_request(struct request_queue *q)
{
	struct request *req;
	unsigned long offset;
	unsigned long num_bytes;

	req = blk_fetch_request(q);
	while (req != NULL) {

		// Non-fs request should be ignored
		if (req == NULL || req->cmd_type != REQ_TYPE_FS) {
			__blk_end_request_all(req, -EIO);
			continue;
		}

		offset = blk_rq_pos(req) * 512;
		num_bytes = blk_rq_cur_sectors(req) * 512;

		if (rq_data_dir(req)) { // write
			if (debug) {
				printk(KERN_INFO "write to sector %u with %u bytes\n",
					   (unsigned int)blk_rq_pos(req), (unsigned int)num_bytes);
			}
			memcpy(_ramdisk_data + offset, req->buffer, num_bytes);
		} else { // read
			if (debug) {
				printk(KERN_INFO "read %u bytes starting from sector %u\n",
					   (unsigned int)num_bytes, (unsigned int)blk_rq_pos(req));
			}
			memcpy(req->buffer, _ramdisk_data + offset, num_bytes);
		}

		// Check if request has been completed. If so, log it.
		if (!__blk_end_request_cur(req, 0)) {
			spin_lock(&_log_lock);

			rw_log[log_ptr].type = rq_data_dir(req);
			rw_log[log_ptr].start_sector = blk_rq_pos(req);
			rw_log[log_ptr].num_bytes = num_bytes;
			rw_log[log_ptr].time = (jiffies * 1000) / HZ;

			++cur_log_size;
			if (cur_log_size > LOG_SIZE) {
				cur_log_size = LOG_SIZE;
			}

			++log_ptr;
			if (log_ptr == LOG_SIZE) {
				log_ptr = 0;
			}


			spin_unlock(&_log_lock);

			req = blk_fetch_request(q);
		}
	}

}

/*
 *	Does nothing
 */
static int ramdisk_media_changed(struct gendisk *gd)
{
	return 0;
}

/*
 *	Does nothing
 */
static int ramdisk_revalidate(struct gendisk *gd)
{
	return 0;
}

/*
 *	Does nothing
 */
static int ramdisk_ioctl(struct block_device *blk_dev, fmode_t f,
						 unsigned int cmd, unsigned long arg)
{
	return 0;
}

/*
 *	Procfs read function -- when read, write log entries to user
 */
static int read_func(char *page, char **start, off_t off, int count, int *eof,
					 void *data)
{
	int len;

	*start = page + off;

	if (!off) {
		_proc_off = 0;
	}

	if (_proc_off >= cur_log_size) {
		*eof = 1;
		return 0;
	}

	len = sprintf(*start, "%u\t%u\t%u\t%u\n",
				  (unsigned int)rw_log[_proc_off].time,
				  (unsigned int)rw_log[_proc_off].type,
				  (unsigned int)rw_log[_proc_off].start_sector,
				  (unsigned int)rw_log[_proc_off].num_bytes);

	++_proc_off;

	return len;
}

module_init(ramdisk_init);
module_exit(ramdisk_cleanup);
