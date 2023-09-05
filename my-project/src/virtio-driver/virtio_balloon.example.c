#include <linux/types.h>
#include <linux/virtio.h>
#include <linux/swap.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/balloon_compaction.h>
#include <linux/cgroup.h>
#include <linux/vmpressure.h>
#include <linux/virtio_config.h>

#include <dev>
#include <virtio_balloon.h>

#define VIRTIO_BALLOON_PAGES_PER_PAGE (unsigned)(PAGE_SIZE >> VIRTIO_BALLOON_PFN_SHIFT)
#define VIRTIO_BALLOON_ARRAY_PFNS_MAX 256
#define VIRTIO_BALLOON_MSG_PRESSURE 1


struct virtio_balloon
{
	struct virtio_device *vdev;
	struct virtqueue *inflate_vq, *deflate_vq, *stats_virtqueue, *message_virtqueue;

	/* Where the ballooning thread waits for config to change. */
	wait_queue_head_t config_change;

	/* The thread servicing the balloon. */
	struct task_struct *thread;

	/* Waiting for host to ack the pages we released. */
	wait_queue_head_t acked;

	wait_queue_head_t message_acked;

	/* Number of balloon pages we've told the Host we're not using. */
	unsigned int num_pages;
	/*
	 * The pages we've told the Host we're not using are enqueued
	 * at vb_dev_info->pages list.
	 * Each page on this list adds VIRTIO_BALLOON_PAGES_PER_PAGE
	 * to num_pages above.
	 */
	struct balloon_dev_info *vb_dev_info;

	/* Synchronize access/update to this struct virtio_balloon elements */
	struct mutex balloon_lock;

	/* The array of pfns we tell the Host about. */
	unsigned int num_pfns;
	u32 pfns[VIRTIO_BALLOON_ARRAY_PFNS_MAX];

	/* Memory statistics */
	int need_stats_update;
	struct virtio_balloon_stat stats[VIRTIO_BALLOON_S_NR];

	/* Message virtqueue */
	atomic_t guest_pressure;
};


static inline bool guest_under_pressure(const struct virtio_balloon *vb){}
static void vmpressure_event_handler(void *data, int level){}
static void tell_host_pressure(struct virtio_balloon *vb){}
static void balloon_ack(struct virtqueue *vq){}
static void message_ack(struct virtqueue *vq){}
static void tell_host(struct virtio_balloon *vb, struct virtqueue *vq){}
static void fill_balloon(struct virtio_balloon *vb, size_t num)
{
	struct balloon_dev_info *vb_dev_info = vb->vb_dev_info;

	/* We can only do one array worth at a time. */
	num = min(num, ARRAY_SIZE(vb->pfns));

	mutex_lock(&vb->balloon_lock);
	for (vb->num_pfns = 0; vb->num_pfns < num;
	     vb->num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE) {
	     	struct page *page;

	     	if (guest_under_pressure(vb)) {
			break;
		}

		page = balloon_page_enqueue(vb_dev_info);
		if (!page) {
			dev_info_ratelimited(&vb->vdev->dev,
					     "Out of puff! Can't get %u pages\n",
					     VIRTIO_BALLOON_PAGES_PER_PAGE);
			/* Sleep for at least 1/5 of a second before retry. */
			msleep(200);
			break;
		}
		add_page_to_balloon_pfns(vb->pfns + vb->num_pfns, page);
		vb->num_pages += VIRTIO_BALLOON_PAGES_PER_PAGE;
		adjust_managed_page_count(page, -1);
	}

	/* Did we get any? */
	if (vb->num_pfns != 0)
		tell_host(vb, vb->inflate_vq);
	mutex_unlock(&vb->balloon_lock);
}
static void release_pages_by_pfn(const u32 pfns[], unsigned int num){}
static void leak_balloon(struct virtio_balloon *vb, size_t num)
{
	struct page *page;
	struct balloon_dev_info *vb_dev_info = vb->vb_dev_info;

	/* We can only do one array worth at a time. */
	num = min(num, ARRAY_SIZE(vb->pfns));

	mutex_lock(&vb->balloon_lock);
	for (vb->num_pfns = 0; vb->num_pfns < num;
	     vb->num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE) {
		page = balloon_page_dequeue(vb_dev_info);
		if (!page)
			break;
		add_page_to_balloon_pfns(vb->pfns + vb->num_pfns, page);
		vb->num_pages -= VIRTIO_BALLOON_PAGES_PER_PAGE;
	}

	/*
	 * Note that if
	 * virtio_has_feature(vdev, VIRTIO_BALLOON_F_MUST_TELL_HOST);
	 * is true, we *have* to do it in this order
	 */
	if (vb->num_pfns != 0)
		tell_host(vb, vb->deflate_vq);
	mutex_unlock(&vb->balloon_lock);
	release_pages_by_pfn(vb->pfns, vb->num_pfns);
}
static void update_stats(struct virtio_balloon *vb){}
static void stats_request(struct virtqueue *vq){}
static void send_stats_to_host(struct virtio_balloon *vb){}
static inline s64 towards_target(struct virtio_balloon *vb)
{
	__le32 v;
	s64 target;

	virtio_cread(vb->vdev, struct virtio_balloon_config, num_pages, &v);

	target = le32_to_cpu(v);
	return target - vb->num_pages;
}
static void update_balloon_size(struct virtio_balloon *vb)
{
	__le32 actual = cpu_to_le32(vb->num_pages);

	virtio_cwrite(vb->vdev, struct virtio_balloon_config, actual,
		      &actual);
}
static int balloon(void *_vballoon)
{
	struct virtio_balloon *vb = _vballoon;

	set_freezable();
	while (!kthread_should_stop()) {
		s64 diff;

		try_to_freeze();
		wait_event_interruptible(vb->config_change,
					 (diff = towards_target(vb)) != 0
					 || guest_under_pressure(vb)
					 || vb->need_stats_update
					 || kthread_should_stop()
					 || freezing(current));
		if (vb->need_stats_update)
			send_stats_to_host(vb);
		if (diff > 0)
			fill_balloon(vb, diff);
		else if (diff < 0)
			leak_balloon(vb, -diff);
		update_balloon_size(vb);

		if (guest_under_pressure(vb))
			tell_host_pressure(vb);

	}
	return 0;
}

/**
 * The function "init_vqs" initializes the virtqueues for the virtio_balloon vdev, including the
 * inflate, deflate, stats, and message virtqueues if supported.
 * 
 * @param vb A pointer to a struct virtio_balloon object.
 * 
 * @return an integer value.
 */
static int init_vqs(struct virtio_balloon *vb)
{
	struct virtqueue *vqs[4];
	vq_callback_t *callbacks[] = { balloon_ack, balloon_ack,
				       stats_request, message_ack };
	const char *names[] = { "inflate", "deflate", "stats", "pressure" };
	int err, nvqs, idx;

	/*
	 * We expect two virtqueues: inflate and deflate, and
	 * optionally stat and message.
	 */
	nvqs = 2 + virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ) +
		virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_MESSAGE_VQ);
	err = vb->vdev->config->find_vqs(vb->vdev, nvqs, vqs, callbacks, names);
	if (err)
		return err;

	idx = 0;
	vb->inflate_vq = vqs[idx++];
	vb->deflate_vq = vqs[idx++];
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ)) {
		struct scatterlist sg;
		vb->stats_virtqueue = vqs[idx++];

		/*
		 * Prime this virtqueue with one buffer so the hypervisor can
		 * use it to signal us later.
		 */
		sg_init_one(&sg, vb->stats, sizeof vb->stats);
		if (virtqueue_add_outbuf(vb->stats_virtqueue, &sg, 1, vb, GFP_KERNEL)
		    < 0)
			BUG();
		virtqueue_kick(vb->stats_virtqueue);
	}
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_MESSAGE_VQ))
		vb->message_virtqueue = vqs[idx];

	return 0;
}

static const struct address_space_operations virtio_balloon_aops;
#ifdef CONFIG_BALLOON_COMPACTION
/*
 * virtballoon_migratepage - perform the balloon page migration on behalf of
 *			     a compation thread.     (called under page lock)
 * @mapping: the page->mapping which will be assigned to the new migrated page.
 * @newpage: page that will replace the isolated page after migration finishes.
 * @page   : the isolated (old) page that is about to be migrated to newpage.
 * @mode   : compaction mode -- not used for balloon page migration.
 *
 * After a ballooned page gets isolated by compaction procedures, this is the
 * function that performs the page migration on behalf of a compaction thread
 * The page migration for virtio balloon is done in a simple swap fashion which
 * follows these two macro steps:
 *  1) insert newpage into vb->pages list and update the host about it;
 *  2) update the host about the old page removed from vb->pages list;
 *
 * This function preforms the balloon page migration task.
 * Called through balloon_mapping->a_ops->migratepage
 */
int virtballoon_migratepage(struct address_space *mapping,
		struct page *newpage, struct page *page, enum migrate_mode mode)
{
	struct balloon_dev_info *vb_dev_info = balloon_page_device(page);
	struct virtio_balloon *vb;
	unsigned long flags;

	BUG_ON(!vb_dev_info);

	vb = vb_dev_info->balloon_device;

	/*
	 * In order to avoid lock contention while migrating pages concurrently
	 * to leak_balloon() or fill_balloon() we just give up the balloon_lock
	 * this turn, as it is easier to retry the page migration later.
	 * This also prevents fill_balloon() getting stuck into a mutex
	 * recursion in the case it ends up triggering memory compaction
	 * while it is attempting to inflate the ballon.
	 */
	if (!mutex_trylock(&vb->balloon_lock))
		return -EAGAIN;

	/* balloon's page migration 1st step  -- inflate "newpage" */
	spin_lock_irqsave(&vb_dev_info->pages_lock, flags);
	balloon_page_insert(newpage, mapping, &vb_dev_info->pages);
	vb_dev_info->isolated_pages--;
	spin_unlock_irqrestore(&vb_dev_info->pages_lock, flags);
	vb->num_pfns = VIRTIO_BALLOON_PAGES_PER_PAGE;
	add_page_to_balloon_pfns(vb->pfns, newpage);
	tell_host(vb, vb->inflate_vq);

	/*
	 * balloon's page migration 2nd step -- deflate "page"
	 *
	 * It's safe to delete page->lru here because this page is at
	 * an isolated migration list, and this step is expected to happen here
	 */
	balloon_page_delete(page);
	vb->num_pfns = VIRTIO_BALLOON_PAGES_PER_PAGE;
	add_page_to_balloon_pfns(vb->pfns, page);
	tell_host(vb, vb->deflate_vq);

	mutex_unlock(&vb->balloon_lock);

	return MIGRATEPAGE_BALLOON_SUCCESS;
}

/* define the balloon_mapping->a_ops callback to allow balloon page migration */
static const struct address_space_operations virtio_balloon_aops = {
			.migratepage = virtballoon_migratepage,
};
#endif /* CONFIG_BALLOON_COMPACTION */


static int virtballoon_probe(struct virtio_device *vdev)
{
	struct virtio_balloon *vb;
	struct address_space *vb_mapping;
	struct balloon_dev_info *vb_devinfo;
	int err;

	vdev->priv = vb = kmalloc(sizeof(*vb), GFP_KERNEL);
	if (!vb) {
		err = -ENOMEM;
		goto out;
	}

	vb->num_pages = 0;
	mutex_init(&vb->balloon_lock);
	init_waitqueue_head(&vb->config_change);
	init_waitqueue_head(&vb->message_acked);
	init_waitqueue_head(&vb->acked);
	vb->vdev = vdev;
	vb->need_stats_update = 0;
	atomic_set(&vb->guest_pressure, 0);

	vb_devinfo = balloon_devinfo_alloc(vb);
	if (IS_ERR(vb_devinfo)) {
		err = PTR_ERR(vb_devinfo);
		goto out_free_vb;
	}

	vb_mapping = balloon_mapping_alloc(vb_devinfo,
					   (balloon_compaction_check()) ?
					   &virtio_balloon_aops : NULL);
	if (IS_ERR(vb_mapping)) {
		/*
		 * IS_ERR(vb_mapping) && PTR_ERR(vb_mapping) == -EOPNOTSUPP
		 * This means !CONFIG_BALLOON_COMPACTION, otherwise we get off.
		 */
		err = PTR_ERR(vb_mapping);
		if (err != -EOPNOTSUPP)
			goto out_free_vb_devinfo;
	}

	vb->vb_dev_info = vb_devinfo;

	err = init_vqs(vb);
	if (err)
		goto out_free_vb_mapping;

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_MESSAGE_VQ)) {
		err = vmpressure_register_kernel_event(NULL, vmpressure_event_handler, vb);
		if (err)
			goto out_free_vb_mapping;
	}

	vb->thread = kthread_run(balloon, vb, "vballoon");
	if (IS_ERR(vb->thread)) {
		err = PTR_ERR(vb->thread);
		goto out_del_vqs;
	}

	return 0;

out_del_vqs:
	/* FIXME: add vmpressure_deregister_kernel_event() */
	vdev->config->del_vqs(vdev);
out_free_vb_mapping:
	balloon_mapping_free(vb_mapping);
out_free_vb_devinfo:
	balloon_devinfo_free(vb_devinfo);
out_free_vb:
	kfree(vb);
out:
	return err;
}

static void remove_common(struct virtio_balloon *vb)
{
	/* There might be pages left in the balloon: free them. */
	while (vb->num_pages)
		leak_balloon(vb, vb->num_pages);
	update_balloon_size(vb);

	/* Now we reset the vdev so we can clean up the queues. */
	vb->vdev->config->reset(vb->vdev);

	vb->vdev->config->del_vqs(vb->vdev);
}

static void virtballoon_remove(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;

	kthread_stop(vb->thread);
	remove_common(vb);
	balloon_mapping_free(vb->vb_dev_info->mapping);
	balloon_devinfo_free(vb->vb_dev_info);
	kfree(vb);
}

#ifdef CONFIG_PM_SLEEP
/**
 * The virtballoon_freeze function removes the virtio_balloon vdev and returns 0.
 * 
 * @param vdev A pointer to a struct virtio_device, which represents a VirtIO vdev.
 * 
 * @return 0.
 */
static int virtballoon_freeze(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;

	/*
	 * The kthread is already frozen by the PM core before this
	 * function is called.
	 */

	remove_common(vb);
	return 0;
}


/**
 * The function virtballoon_restore initializes the virtual queues, fills the balloon, and updates the
 * balloon size.
 * 
 * @param vdev A pointer to the virtio_device structure representing the virtio vdev.
 * 
 * @return an integer value.
 */
static int virtballoon_restore(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;
	int ret;

	ret = init_vqs(vdev->priv);
	if (ret)
		return ret;

	fill_balloon(vb, towards_target(vb));
	update_balloon_size(vb);
	return 0;
}
#endif

static unsigned int features[] = {
	VIRTIO_BALLOON_F_MUST_TELL_HOST,
	VIRTIO_BALLOON_F_STATS_VQ,
	VIRTIO_BALLOON_F_MESSAGE_VQ,
};

static struct virtio_driver virtio_balloon_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name =	KBUILD_MODNAME,
	.driver.owner =	THIS_MODULE,
	.id_table =	id_table,
	.probe =	virtballoon_probe,
	.remove =	virtballoon_remove,
#ifdef CONFIG_PM_SLEEP
	.freeze	=	virtballoon_freeze,
	.restore =	virtballoon_restore,
#endif
};

module_virtio_driver(virtio_balloon_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio balloon driver");
MODULE_LICENSE("GPL");