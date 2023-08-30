#include <linux/types.h>
#include <linux/virtio.h>
#include <linux/virtio_balloon.h>
#include <linux/swap.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/balloon_compaction.h>

#include <linux/cgroup.h>
#include <linux/vmpressure.h>


/*
 * Balloon device works in 4K page units.  So each page is pointed to by
 * multiple balloon pages.  All memory counters in this driver are in balloon
 * page units.
 */
#define VIRTIO_BALLOON_PAGES_PER_PAGE (unsigned)(PAGE_SIZE >> VIRTIO_BALLOON_PFN_SHIFT)
#define VIRTIO_BALLOON_ARRAY_PFNS_MAX 256

#define VIRTIO_BALLOON_MSG_PRESSURE 1

struct virtio_balloon
{
	struct virtio_device *device;
	struct virtqueue *inflate_vq, *deflate_vq, *stats_vq, *message_virtqueue;

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

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_BALLOON, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static inline bool guest_under_pressure(const struct virtio_balloon *vb)
{
	return atomic_read(&vb->guest_pressure) == 1;
}

static void vmpressure_event_handler(void *data, int level)
{
	struct virtio_balloon *vb = data;

	atomic_set(&vb->guest_pressure, 1);
	wake_up(&vb->config_change);
}

static void tell_host_pressure(struct virtio_balloon *vb)
{
	const uint32_t msg = VIRTIO_BALLOON_MSG_PRESSURE;
	struct scatterlist sg;
	unsigned int len;
	int err;

	sg_init_one(&sg, &msg, sizeof(msg));

	err = virtqueue_add_outbuf(vb->message_virtqueue, &sg, 1, vb, GFP_KERNEL);
	if (err < 0) {
		printk(KERN_WARNING "virtio-balloon: failed to send host message (%d)\n", err);
		goto out;
	}
	virtqueue_kick(vb->message_virtqueue);

	wait_event(vb->message_acked, virtqueue_get_buf(vb->message_virtqueue, &len));

out:
	atomic_set(&vb->guest_pressure, 0);
}

static u32 page_to_balloon_pfn(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);

	BUILD_BUG_ON(PAGE_SHIFT < VIRTIO_BALLOON_PFN_SHIFT);
	/* Convert pfn from Linux page size to balloon page size. */
	return pfn * VIRTIO_BALLOON_PAGES_PER_PAGE;
}

static struct page *balloon_pfn_to_page(u32 pfn)
{
	BUG_ON(pfn % VIRTIO_BALLOON_PAGES_PER_PAGE);
	return pfn_to_page(pfn / VIRTIO_BALLOON_PAGES_PER_PAGE);
}

static void balloon_ack(struct virtqueue *vq)
{
	struct virtio_balloon *vb = vq->device->priv;

	wake_up(&vb->acked);
}

static void message_ack(struct virtqueue *vq)
{
	struct virtio_balloon *vb = vq->device->priv;

	wake_up(&vb->message_acked);
}

static void tell_host(struct virtio_balloon *vb, struct virtqueue *vq)
{
	struct scatterlist sg;
	unsigned int len;

	sg_init_one(&sg, vb->pfns, sizeof(vb->pfns[0]) * vb->num_pfns);

	/* We should always be able to add one buffer to an empty queue. */
	if (virtqueue_add_outbuf(vq, &sg, 1, vb, GFP_KERNEL) < 0)
		BUG();
	virtqueue_kick(vq);

	/* When host has read buffer, this completes via balloon_ack */
	wait_event(vb->acked, virtqueue_get_buf(vq, &len));
}

static void set_page_pfns(u32 pfns[], struct page *page)
{
	unsigned int i;

	/* Set balloon pfns pointing at this page.
	 * Note that the first pfn points at start of the page. */
	for (i = 0; i < VIRTIO_BALLOON_PAGES_PER_PAGE; i++)
		pfns[i] = page_to_balloon_pfn(page) + i;
}

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
			dev_info_ratelimited(&vb->device->dev,
					     "Out of puff! Can't get %u pages\n",
					     VIRTIO_BALLOON_PAGES_PER_PAGE);
			/* Sleep for at least 1/5 of a second before retry. */
			msleep(200);
			break;
		}
		set_page_pfns(vb->pfns + vb->num_pfns, page);
		vb->num_pages += VIRTIO_BALLOON_PAGES_PER_PAGE;
		adjust_managed_page_count(page, -1);
	}

	/* Did we get any? */
	if (vb->num_pfns != 0)
		tell_host(vb, vb->inflate_vq);
	mutex_unlock(&vb->balloon_lock);
}

static void release_pages_by_pfn(const u32 pfns[], unsigned int num)
{
	unsigned int i;

	/* Find pfns pointing at start of each page, get pages and free them. */
	for (i = 0; i < num; i += VIRTIO_BALLOON_PAGES_PER_PAGE) {
		struct page *page = balloon_pfn_to_page(pfns[i]);
		balloon_page_free(page);
		adjust_managed_page_count(page, 1);
	}
}

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
		set_page_pfns(vb->pfns + vb->num_pfns, page);
		vb->num_pages -= VIRTIO_BALLOON_PAGES_PER_PAGE;
	}

	/*
	 * Note that if
	 * virtio_has_feature(device, VIRTIO_BALLOON_F_MUST_TELL_HOST);
	 * is true, we *have* to do it in this order
	 */
	if (vb->num_pfns != 0)
		tell_host(vb, vb->deflate_vq);
	mutex_unlock(&vb->balloon_lock);
	release_pages_by_pfn(vb->pfns, vb->num_pfns);
}

static inline void update_stat(struct virtio_balloon *vb, int idx,
			       u16 tag, u64 val)
{
	BUG_ON(idx >= VIRTIO_BALLOON_S_NR);
	vb->stats[idx].tag = tag;
	vb->stats[idx].val = val;
}

#define pages_to_bytes(x) ((u64)(x) << PAGE_SHIFT)

static void update_balloon_stats(struct virtio_balloon *vb)
{
	unsigned long events[NR_VM_EVENT_ITEMS];
	struct sysinfo i;
	int idx = 0;

	all_vm_events(events);
	si_meminfo(&i);

	update_stat(vb, idx++, VIRTIO_BALLOON_S_SWAP_IN,
				pages_to_bytes(events[PSWPIN]));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_SWAP_OUT,
				pages_to_bytes(events[PSWPOUT]));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MAJFLT, events[PGMAJFAULT]);
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MINFLT, events[PGFAULT]);
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MEMFREE,
				pages_to_bytes(i.freeram));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MEMTOT,
				pages_to_bytes(i.totalram));
}

/*
 * While most virtqueues communicate guest-initiated requests to the hypervisor,
 * the stats queue operates in reverse.  The driver initializes the virtqueue
 * with a single buffer.  From that point forward, all conversations consist of
 * a hypervisor request (a call to this function) which directs us to refill
 * the virtqueue with a fresh stats buffer.  Since stats collection can sleep,
 * we notify our kthread which does the actual work via stats_handle_request().
 */
static void stats_request(struct virtqueue *vq)
{
	struct virtio_balloon *vb = vq->device->priv;

	vb->need_stats_update = 1;
	wake_up(&vb->config_change);
}

static void stats_handle_request(struct virtio_balloon *vb)
{
	struct virtqueue *vq;
	struct scatterlist sg;
	unsigned int len;

	vb->need_stats_update = 0;
	update_balloon_stats(vb);

	vq = vb->stats_vq;
	if (!virtqueue_get_buf(vq, &len))
		return;
	sg_init_one(&sg, vb->stats, sizeof(vb->stats));
	if (virtqueue_add_outbuf(vq, &sg, 1, vb, GFP_KERNEL) < 0)
		BUG();
	virtqueue_kick(vq);
}

static void virtballoon_changed(struct virtio_device *device)
{
	struct virtio_balloon *vb = device->priv;

	wake_up(&vb->config_change);
}

/**
 * The function calculates the difference between the target number of pages and the current number of
 * pages in a virtio balloon device.
 * 
 * @param vb A pointer to a struct virtio_balloon object.
 * 
 * @return the difference between the target number of pages and the current number of pages in the
 * virtio_balloon structure.
 */
static inline s64 towards_target(struct virtio_balloon *vb)
{
	__le32 v;
	s64 target;

	virtio_cread(vb->device, struct virtio_balloon_config, num_pages, &v);

	target = le32_to_cpu(v);
	return target - vb->num_pages;
}

static void update_balloon_size(struct virtio_balloon *vb)
{
	__le32 actual = cpu_to_le32(vb->num_pages);

	virtio_cwrite(vb->device, struct virtio_balloon_config, actual,
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
			stats_handle_request(vb);
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
 * The function "init_vqs" initializes the virtqueues for the virtio_balloon device, including the
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
	nvqs = 2 + virtio_has_feature(vb->device, VIRTIO_BALLOON_F_STATS_VQ) +
		virtio_has_feature(vb->device, VIRTIO_BALLOON_F_MESSAGE_VQ);
	err = vb->device->config->find_vqs(vb->device, nvqs, vqs, callbacks, names);
	if (err)
		return err;

	idx = 0;
	vb->inflate_vq = vqs[idx++];
	vb->deflate_vq = vqs[idx++];
	if (virtio_has_feature(vb->device, VIRTIO_BALLOON_F_STATS_VQ)) {
		struct scatterlist sg;
		vb->stats_vq = vqs[idx++];

		/*
		 * Prime this virtqueue with one buffer so the hypervisor can
		 * use it to signal us later.
		 */
		sg_init_one(&sg, vb->stats, sizeof vb->stats);
		if (virtqueue_add_outbuf(vb->stats_vq, &sg, 1, vb, GFP_KERNEL)
		    < 0)
			BUG();
		virtqueue_kick(vb->stats_vq);
	}
	if (virtio_has_feature(vb->device, VIRTIO_BALLOON_F_MESSAGE_VQ))
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
	set_page_pfns(vb->pfns, newpage);
	tell_host(vb, vb->inflate_vq);

	/*
	 * balloon's page migration 2nd step -- deflate "page"
	 *
	 * It's safe to delete page->lru here because this page is at
	 * an isolated migration list, and this step is expected to happen here
	 */
	balloon_page_delete(page);
	vb->num_pfns = VIRTIO_BALLOON_PAGES_PER_PAGE;
	set_page_pfns(vb->pfns, page);
	tell_host(vb, vb->deflate_vq);

	mutex_unlock(&vb->balloon_lock);

	return MIGRATEPAGE_BALLOON_SUCCESS;
}

/* define the balloon_mapping->a_ops callback to allow balloon page migration */
static const struct address_space_operations virtio_balloon_aops = {
			.migratepage = virtballoon_migratepage,
};
#endif /* CONFIG_BALLOON_COMPACTION */

static int virtballoon_probe(struct virtio_device *device)
{
	struct virtio_balloon *vb;
	struct address_space *vb_mapping;
	struct balloon_dev_info *vb_devinfo;
	int err;

	device->priv = vb = kmalloc(sizeof(*vb), GFP_KERNEL);
	if (!vb) {
		err = -ENOMEM;
		goto out;
	}

	vb->num_pages = 0;
	mutex_init(&vb->balloon_lock);
	init_waitqueue_head(&vb->config_change);
	init_waitqueue_head(&vb->message_acked);
	init_waitqueue_head(&vb->acked);
	vb->device = device;
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

	if (virtio_has_feature(vb->device, VIRTIO_BALLOON_F_MESSAGE_VQ)) {
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
	device->config->del_vqs(device);
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

	/* Now we reset the device so we can clean up the queues. */
	vb->device->config->reset(vb->device);

	vb->device->config->del_vqs(vb->device);
}

static void virtballoon_remove(struct virtio_device *device)
{
	struct virtio_balloon *vb = device->priv;

	kthread_stop(vb->thread);
	remove_common(vb);
	balloon_mapping_free(vb->vb_dev_info->mapping);
	balloon_devinfo_free(vb->vb_dev_info);
	kfree(vb);
}

#ifdef CONFIG_PM_SLEEP
/**
 * The virtballoon_freeze function removes the virtio_balloon device and returns 0.
 * 
 * @param device A pointer to a struct virtio_device, which represents a VirtIO device.
 * 
 * @return 0.
 */
static int virtballoon_freeze(struct virtio_device *device)
{
	struct virtio_balloon *vb = device->priv;

	/*
	 * The kthread is already frozen by the PM core before this
	 * function is called.
	 */

	remove_common(vb);
	return 0;
}

/**
 * The function virtballoon_restore initializes the virtual queues, fills the balloon with memory, and
 * updates the balloon size.
 * 
 * @param device A pointer to a struct virtio_device, which represents a VirtIO device.
 * 
 * @return an integer value.
 */
static int virtballoon_restore(struct virtio_device *device)
{
	struct virtio_balloon *vb = device->priv;
	int ret;

	ret = init_vqs(device->priv);
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
	.config_changed = virtballoon_changed,
#ifdef CONFIG_PM_SLEEP
	.freeze	=	virtballoon_freeze,
	.restore =	virtballoon_restore,
#endif
};

module_virtio_driver(virtio_balloon_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio balloon driver");
MODULE_LICENSE("GPL");