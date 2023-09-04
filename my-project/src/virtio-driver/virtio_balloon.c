#include <linux/swap.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/virtio.h>
#include <linux/module.h>
#include <linux/cgroup.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/virtio_ids.h>
#include <linux/vmpressure.h>
#include <linux/virtio_config.h>
#include <linux/balloon_compaction.h>

#include <virtio_balloon.h>

#define VIRTIO_BALLOON_PAGES_PER_PAGE (unsigned)(PAGE_SIZE >> VIRTIO_BALLOON_PFN_SHIFT)
#define VIRTIO_BALLOON_ARRAY_PFNS_MAX 256
#define VIRTIO_BALLOON_MSG_PRESSURE 1


/**
 * The struct virtio_balloon represents VirtIO Memory Balloon driver
 * 
 * @vdev: The virtual vdev that the virtio_balloon is associated with.
 * @message_virtqueue: A virtqueue is used for sending and receiving messages 
 * between the host and guest in a virtualized environment.
 * @stats_virtqueue: send memory statistics
 * 
 * @guest_pressure: Represents the pressure from the guest on the vdev
 */
struct virtio_balloon 
{
    struct virtio_device *vdev;
    struct balloon_dev_info *dev_info;
    struct virtqueue *message_virtqueue, *stats_virtqueue;

    /* Where the ballooning thread waits for config to change. */
	wait_queue_head_t config_change;

    /* Memory statistics */
	struct virtio_balloon_stat stats[VIRTIO_BALLOON_S_NR];

    atomic_t guest_pressure;
};


static inline bool guest_under_pressure(const struct virtio_balloon *balloon)
{
	return atomic_read(&balloon->guest_pressure);
}


static void vmpressure_event_handler(void *data, int level)
{
	struct virtio_balloon *balloon = data;

	atomic_set(&balloon->guest_pressure, 1);
	wake_up(&balloon->config_change);
}


static void virtio_balloon_recv_cb(struct virtqueue *vq)
{
    struct virtio_balloon *balloon = vq->vdev->priv;
    char *buf;
    unsigned int len;

    while ((buf = virtqueue_get_buf(balloon->vq, &len)) != NULL) {
            /* process the received data */
    }
}


static int virtio_balloon_probe(struct virtio_device *vdev)
{
    struct virtio_balloon *balloon = NULL;

    /* initialize vdev data */
    balloon = kzalloc(sizeof(struct virtio_balloon), GFP_KERNEL);

    if (!balloon) return -ENOMEM;

    vdev->priv = balloon;
    balloon->vdev = vdev;

    atomic_set(&balloon->guest_pressure, 0);
	/* end init vdev data */

    /* register virtqueues */
    balloon->vq = virtio_find_single_vq(vdev, virtio_balloon_recv_cb, "input");
    if (IS_ERR(balloon->vq)) {
            kfree(balloon);
            return PTR_ERR(balloon->vq);

    }

    /* from this point on, the vdev can notify and get callbacks */
    virtio_device_ready(vdev);

    return 0;
}


static void virtio_balloon_remove(struct virtio_device *vdev)
{
    struct virtio_balloon *balloon = vdev->priv;

    /*
        * disable vq interrupts: equivalent to
        * vdev->config->reset(vdev)
        */
    virtio_reset_device(vdev);

    /* detach unused buffers */
    while ((buf = virtqueue_detach_unused_buf(balloon->vq)) != NULL) {
            kfree(buf);
    }

    /* remove virtqueues */
    vdev->config->del_vqs(vdev);

    kfree(balloon);
}


static const struct virtio_device_id id_table[] = {
    { VIRTIO_ID_BALLOON, VIRTIO_DEV_ANY_ID },
	{ 0 },
};


static struct virtio_driver virtio_balloon_driver = {
    .driver.name =    KBUILD_MODNAME,
    .driver.owner =   THIS_MODULE,
    .id_table =       id_table,
    .probe =          virtio_balloon_probe,
    .remove =         virtio_balloon_remove,
    .config_changed = virtballoon_changed,
};


module_virtio_driver(virtio_balloon_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio balloon driver");
MODULE_LICENSE("GPL");


// ******************** UTILS ********************

// *** Driver Func ***
static void virtballoon_changed(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;
	wake_up(&vb->config_change);
}
// *** End Driver Func ***

// *** Balloon Func ***
static void inflate_balloon(struct virtio_balloon *balloon){
    struct page *new_page = balloon_page_alloc();

    if (!new_page) return;

    balloon_page_enqueue(balloon->dev_info, new_page);
}

static void deflate_balloon(struct virtio_balloon *balloon){
    
}
// *** End Balloon Func ***

// *** Comunication Func ***
static void send_to_host(struct virtqueue *vq, void *message, wait_queue_head_t ack)
{
	struct scatterlist sg;
	unsigned int len;

	if (!virtqueue_get_buf(vq, &len)) return;

	sg_init_one(&sg, message, sizeof(message));

    struct virtio_balloon *vb = vq->vdev->priv;

	if (virtqueue_add_outbuf(vq, &sg, 1, vb, GFP_KERNEL) < 0) BUG();
    // notify host
	virtqueue_kick(vq);

    if (ack) wait_event(ack, virtqueue_get_buf(vq, &len));
}
// *** End Comunication Func ***


// *** Page Utils ***
static struct page *balloon_pfn_to_page(u32 pfn)
{
	BUG_ON(pfn % VIRTIO_BALLOON_PAGES_PER_PAGE);
	return pfn_to_page(pfn / VIRTIO_BALLOON_PAGES_PER_PAGE);
}


static void add_page_to_balloon_pfns(u32 pfns[], struct page *page)
{
	BUILD_BUG_ON(PAGE_SHIFT < VIRTIO_BALLOON_PFN_SHIFT);

	unsigned long pfn = page_to_pfn(page);
	u32 start_balloon_pfn = pfn * VIRTIO_BALLOON_PAGES_PER_PAGE;

	for (unsigned int i = 0; i < VIRTIO_BALLOON_PAGES_PER_PAGE; i++)
		pfns[i] = start_balloon_pfn + i;
}
// *** End Page Utils ***


// *** Update Stats ***
static inline void write_stat_to_balloon(struct virtio_balloon *vb, int idx,
			       u16 tag, u64 val)
{
	vb->stats[idx].tag = tag;
	vb->stats[idx].val = val;
}

#define pages_to_bytes(x) ((u64)(x) << PAGE_SHIFT)

static void update_stats(struct virtio_balloon *vb)
{
	struct sysinfo i;
	int idx = 0;

	si_meminfo(&i);

	write_stat_to_balloon(vb, idx++, VIRTIO_BALLOON_S_MEMFREE,
				pages_to_bytes(i.freeram));
	write_stat_to_balloon(vb, idx++, VIRTIO_BALLOON_S_MEMTOT,
				pages_to_bytes(i.totalram));

    send_to_host(vb->stats_virtqueue, vb->stats, NULL);
}
// *** End Update Stats ***
