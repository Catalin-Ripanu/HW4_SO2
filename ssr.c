// SPDX-License-Identifier: GPL-2.0+

/*
 * Linux RAID block device driver
 *
 * Author: Cătălin-Alexandru Rîpanu catalin.ripanu@stud.acs.upb.ro
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/genhd.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/blk_types.h>
#include <linux/bio.h>
#include <linux/vmalloc.h>
#include <linux/crc32.h>
#include <linux/workqueue.h>

#include "ssr.h"

#define LOGICAL_DEV_NAME "ssr"

struct logical_block_dev {
	struct blk_mq_tag_set tag_set;
	struct request_queue *queue;
	struct gendisk *gd;
	size_t size;
};

struct ssr_work {
	struct work_struct work;
	char *buffer_from_up;
	struct bio *bio_from_up;
	struct bio *data_bio_from_down;
	struct bio *crc32_bio_from_down;
};

static struct workqueue_struct *ssr_wq;

static struct logical_block_dev logical_raid_block_device;

static struct block_device *phys_bdev_vdb;
static struct block_device *phys_bdev_vdc;

/**
 * ssr_block_open - block_device open operation
 * @bdev: block_device structure containing the device information
 * @mode: mode in which the device is to be opened
 *
 * This function is called when the block device is opened.
 * Currently, it performs no specific action and always returns 0.
 */
static int ssr_block_open(struct block_device *bdev, fmode_t mode)
{
	return 0;
}

/**
 * ssr_block_release - block_device release operation
 * @gd: gendisk structure containing the disk information
 * @mode: mode in which the device was opened
 *
 * This function is called when the block device is released.
 * Currently, it performs no specific action.
 */
static void ssr_block_release(struct gendisk *gd, fmode_t mode)
{
}

/**
 * process_device - Processes the read/write requests on a given device
 * @bio_from_up: Bio structure representing the original request
 * @data_bio_from_down: Bio structure representing the data part from physical device sector
 * @crc32_bio_from_down: Bio structure representing the CRC32 part from physical device sector
 *
 * This function processes the bio segments, calculates CRC32 checksums,
 * and verifies their integrity. It handles both read and write operations.
 */
static void process_device(struct bio *bio_from_up, struct bio *data_bio_from_down, struct bio *crc32_bio_from_down)
{
	int dir = bio_data_dir(bio_from_up);

	struct bio_vec data_bvec, crc32_bvec;
	struct bvec_iter data_iter;
	u32 crc_vdb, crc_vdc;

	bio_for_each_segment(data_bvec, data_bio_from_down, data_iter) {

		size_t size;
		unsigned long data_offset;
		unsigned long crc32_offset;
		char *data_buffer, *crc32_buffer;

		crc32_bvec = bio_iter_iovec(crc32_bio_from_down, data_iter);

		size = data_bvec.bv_len;
		data_offset = data_bvec.bv_offset;
		crc32_offset = crc32_bvec.bv_offset;

		data_buffer = kmap_atomic(data_bvec.bv_page);
		crc32_buffer = kmap_atomic(crc32_bvec.bv_page);

		crc_vdb = crc32(0, data_buffer + data_offset, size);
		crc_vdc = crc32(0, crc32_buffer + crc32_offset, size);

		if (crc_vdb != crc_vdc) {
			kunmap_atomic(data_buffer);
			kunmap_atomic(crc32_buffer);
			return;
		}

		if (dir == REQ_OP_READ) {
			char *buffer_from_up = kmap_atomic(data_bvec.bv_page);

			memcpy(buffer_from_up + data_offset, data_buffer + data_offset, size);
			memcpy(buffer_from_up + crc32_offset, crc32_buffer + crc32_offset, size);

			kunmap_atomic(buffer_from_up);

		} else if (dir == REQ_OP_WRITE) {
			memcpy(data_buffer + data_offset, data_buffer + data_offset, size);
			memcpy(crc32_buffer + crc32_offset, data_buffer + crc32_offset, size);
		}

		kunmap_atomic(data_buffer);
		kunmap_atomic(crc32_buffer);
	}
}

/**
 * ssr_handle_requests - Handles read and write requests for the RAID logical block device
 * @work: Work structure containing the request data
 *
 * This function is executed in a workqueue context. It handles bio
 * requests by processing them for both primary and CRC32 verification.
 */
static void ssr_handle_requests(struct work_struct *work)
{
	struct ssr_work *ssrwork = container_of(work, struct ssr_work, work);
	struct bio *bio_from_up = ssrwork->bio_from_up;
	struct bio *data_bio_from_down = ssrwork->data_bio_from_down;
	struct bio *crc32_bio_from_down = ssrwork->crc32_bio_from_down;

	data_bio_from_down->bi_disk = phys_bdev_vdb->bd_disk;
	crc32_bio_from_down->bi_disk = phys_bdev_vdb->bd_disk;

	process_device(bio_from_up, data_bio_from_down, crc32_bio_from_down);

	data_bio_from_down->bi_disk = phys_bdev_vdc->bd_disk;
	crc32_bio_from_down->bi_disk = phys_bdev_vdc->bd_disk;

	process_device(bio_from_up, data_bio_from_down, crc32_bio_from_down);

	submit_bio_wait(crc32_bio_from_down);
	submit_bio_wait(data_bio_from_down);

	kfree(ssrwork);
	bio_endio(bio_from_up);
}

/**
 * ssr_submit_bio - Submits a bio request to the RAID logical block device
 * @bio_from_up: Bio structure representing the request
 *
 * This function allocates bio structures for data and CRC32 operations,
 * maps the bio segments, and queues the request for processing.
 *
 * Returns a blk_qc_t value indicating the status of the request.
 */
static blk_qc_t ssr_submit_bio(struct bio *bio_from_up)
{
	struct bio *data_bio_from_down, *crc32_bio_from_down;
	struct bio_vec bvec;
	struct bvec_iter iter;
	struct ssr_work *ssrwork;
	struct page *data_page, *crc32_page;
	struct completion comp;
	int ret = BLK_QC_T_NONE;
	int dir = bio_data_dir(bio_from_up);

	data_bio_from_down = bio_alloc(GFP_NOIO, bio_from_up->bi_vcnt);

	if (!data_bio_from_down) {
		ret = BLK_STS_RESOURCE;
		goto out;
	}

	crc32_bio_from_down = bio_alloc(GFP_NOIO, bio_from_up->bi_vcnt);

	if (!crc32_bio_from_down) {
		ret = BLK_STS_RESOURCE;
		goto crc32_bio_fail;
	}

	bio_for_each_segment(bvec, bio_from_up, iter) {
		sector_t sector = iter.bi_sector;
		unsigned long offset = bvec.bv_offset;
		size_t len = bvec.bv_len;
		char *buffer_from_up = kmap(bvec.bv_page);

		data_bio_from_down->bi_iter.bi_sector = sector;
		crc32_bio_from_down->bi_iter.bi_sector = sector;

		data_bio_from_down->bi_opf = dir;
		crc32_bio_from_down->bi_opf = dir;

		data_page = alloc_page(GFP_NOIO);

		if (!data_page) {
			ret = BLK_STS_RESOURCE;
			goto data_page_fail;
		}

		crc32_page = alloc_page(GFP_NOIO);

		if (!crc32_page) {
			ret = BLK_STS_RESOURCE;
			goto crc32_page_fail;
		}

		bio_add_page(data_bio_from_down, data_page, len, offset);

		bio_add_page(crc32_bio_from_down, crc32_page, len, offset);

		ssrwork = kmalloc(sizeof(*ssrwork), GFP_KERNEL);
		if (!ssrwork) {
			ret = BLK_STS_RESOURCE;
			goto ssrwork_failed;
		}

		INIT_WORK(&ssrwork->work, ssr_handle_requests);
		ssrwork->bio_from_up = bio_from_up;
		ssrwork->data_bio_from_down = data_bio_from_down;
		ssrwork->crc32_bio_from_down = crc32_bio_from_down;
		ssrwork->buffer_from_up = buffer_from_up + offset;
		queue_work(ssr_wq, &ssrwork->work);

		kunmap(bvec.bv_page);

		return ret;
	}

ssrwork_failed:
crc32_page_fail:
	__free_page(crc32_page);
data_page_fail:
	bio_put(crc32_bio_from_down);
crc32_bio_fail:
	bio_put(data_bio_from_down);
out:
	bio_endio(bio_from_up);
	return ret;
}

/**
 * ssr_block_ops - Block device operations for the RAID logical block device
 *
 * This structure defines the operations that can be performed on the
 * RAID logical block device, including open, release, and submit_bio.
 */
static const struct block_device_operations ssr_block_ops = {
	.owner = THIS_MODULE,
	.open = ssr_block_open,
	.release = ssr_block_release,
	.submit_bio = ssr_submit_bio,
};

/**
 * create_block_device - Initializes and creates the logical block device
 * @dev: Pointer to the logical_block_dev structure representing the device
 *
 * This function sets up the logical block device, including allocation of the
 * request queue, setting the logical block size, initializing the gendisk structure,
 * and adding the disk to the system.
 *
 * Returns 0 on success or a negative error code on failure.
 */
static int create_block_device(struct logical_block_dev *dev)
{
	int err;

	dev->size = LOGICAL_DISK_SIZE;

	dev->queue = blk_alloc_queue(NUMA_NO_NODE);

	if (!dev->queue) {
		pr_err("blk_alloc_queue: failure\n");
		err = -ENOMEM;
		goto out_vmalloc;
	}

	blk_queue_logical_block_size(dev->queue, KERNEL_SECTOR_SIZE);
	dev->queue->queuedata = dev;

	dev->gd = alloc_disk(SSR_NUM_MINORS);

	if (!dev->gd) {
		pr_err("alloc_disk: failure\n");
		err = -ENOMEM;
		goto out_alloc_disk;
	}

	dev->gd->major = SSR_MAJOR;
	dev->gd->first_minor = SSR_FIRST_MINOR;
	dev->gd->fops = &ssr_block_ops;
	dev->gd->queue = dev->queue;
	dev->gd->private_data = dev;
	snprintf(dev->gd->disk_name, DISK_NAME_LEN, LOGICAL_DEV_NAME);
	set_capacity(dev->gd, LOGICAL_DISK_SECTORS);

	add_disk(dev->gd);

	return 0;

out_alloc_disk:
	blk_cleanup_queue(dev->queue);
out_vmalloc:
	return err;
}

/**
 * open_disk - Opens a physical block device by its name
 * @name: Name of the physical block device to open
 *
 * This function opens the specified block device with read and write permissions,
 * and exclusive access. It returns a pointer to the block_device structure representing
 * the opened device, or NULL if the device could not be opened.
 *
 * Returns a pointer to the block_device structure on success, or NULL on failure.
 */
static struct block_device *open_disk(char *name)
{
	struct block_device *bdev;

	bdev = blkdev_get_by_path(name, FMODE_READ | FMODE_WRITE | FMODE_EXCL,
							  THIS_MODULE);
	if (IS_ERR(bdev))
		return NULL;

	return bdev;
}

/**
 * close_disk - Closes a previously opened block device
 * @bdev: Pointer to the block_device structure representing the device
 *
 * This function releases the block device that was previously opened with
 * open_disk(), freeing any associated resources.
 */
static void close_disk(struct block_device *bdev)
{
	blkdev_put(bdev, FMODE_READ | FMODE_WRITE | FMODE_EXCL);
}

/**
 * delete_block_device - Cleans up and deletes the logical block device
 * @dev: Pointer to the logical_block_dev structure representing the device
 *
 * This function deletes the logical block device by removing the gendisk structure
 * and cleaning up the request queue.
 */
static void delete_block_device(struct logical_block_dev *dev)
{
	if (dev->gd) {
		del_gendisk(dev->gd);
		put_disk(dev->gd);
	}

	if (dev->queue)
		blk_cleanup_queue(dev->queue);
}

/**
 * ssr_init - Module initialization function
 *
 * This function is called when the module is loaded. It creates the workqueue,
 * registers the block device, and initializes the logical block device.
 *
 * Returns 0 on success or a negative error code on failure.
 */
static int __init ssr_init(void)
{
	int err = 0;

	ssr_wq = create_singlethread_workqueue("ssr_workqueue");
	if (!ssr_wq) {
		pr_err("create_singlethread_workqueue: failure\n");
		return -ENOMEM;
	}

	err = register_blkdev(SSR_MAJOR, LOGICAL_DEV_NAME);
	if (err < 0) {
		pr_err("register_blkdev: unable to register\n");
		destroy_workqueue(ssr_wq);
		return err;
	}

	err = create_block_device(&logical_raid_block_device);
	if (err < 0)
		goto out_register_blkdev;

	phys_bdev_vdb = open_disk(PHYSICAL_DISK1_NAME);
	if (phys_bdev_vdb == NULL) {
		pr_err("open_disk: No such device (%s)\n",
			   PHYSICAL_DISK1_NAME);
		err = -EINVAL;
		goto out_block_device;
	}

	phys_bdev_vdc = open_disk(PHYSICAL_DISK2_NAME);
	if (phys_bdev_vdc == NULL) {
		pr_err("open_disk: No such device (%s)\n",
			   PHYSICAL_DISK2_NAME);
		err = -EINVAL;
		goto out_vdb_open_disk;
	}

	return 0;

out_vdb_open_disk:
	close_disk(phys_bdev_vdb);
out_block_device:
	delete_block_device(&logical_raid_block_device);
out_register_blkdev:
	unregister_blkdev(SSR_MAJOR, LOGICAL_DEV_NAME);
	destroy_workqueue(ssr_wq);
	return err;
}

/**
 * ssr_exit - Module cleanup function
 *
 * This function is called when the module is unloaded. It destroys the workqueue,
 * deletes the logical block device, closes the physical block devices, and unregisters
 * the block device.
 */
static void __exit ssr_exit(void)
{
	flush_workqueue(ssr_wq);
	destroy_workqueue(ssr_wq);

	delete_block_device(&logical_raid_block_device);
	close_disk(phys_bdev_vdb);
	close_disk(phys_bdev_vdc);

	unregister_blkdev(SSR_MAJOR, LOGICAL_DEV_NAME);
}

module_init(ssr_init);
module_exit(ssr_exit);

MODULE_DESCRIPTION("RAID logical block device implementation");
MODULE_AUTHOR("Catalin-Alexandru Ripanu catalin.ripanu@stud.acs.upb.ro");
MODULE_LICENSE("GPL v2");
