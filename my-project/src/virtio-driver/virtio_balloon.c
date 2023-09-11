#include <linux/err.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/list.h>
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

#include "virtio_balloon.h"

#define VIRTIO_BALLOON_PAGES_PER_32MB (32 << 8)


struct virtio_balloon 
{
    struct virtio_device *vdev;
    struct balloon_dev_info *balloon_dev_info;
    struct virt_channel *stats_channel, *inflate_channel, *deflate_channel;

    /* The thread servicing the balloon. */
	struct task_struct *thread;

    /* Ensure only one thread operates pages at a time */
    struct mutex page_mutex;

    // Number of balloon pages give to host
    unsigned int num_pages;

    u64 stats[2];
};


// ******************** UTILS ********************
static inline void ack(struct virt_channel *channel) {
    wake_up(channel->ack);
};

static void callback(struct virtqueue *vq) {
    struct virt_channel *channel = vq->priv;
    ack(channel);
}

struct virt_channel *create_virt_channel(struct virtio_device *vdev, char *name) {
    struct virt_channel *channel;

    if (!(channel = kzalloc(sizeof(struct virt_channel), GFP_KERNEL))) 
        return NULL;
    if (!(channel->ack = kzalloc(sizeof(wait_queue_head_t), GFP_KERNEL))) 
        goto out;

    channel->vq = virtio_find_single_vq(vdev, callback, name);
    if (IS_ERR(channel->vq)) 
        goto out_free_ack;

    channel->vq->priv = channel;
    init_waitqueue_head(channel->ack);
    return channel;

out_free_ack:
    kfree(channel->ack);
out:
    kfree(channel);
    return NULL;
};

void free_channel_buf(struct virt_channel *channel) {
    void *buf;
    while ((buf = virtqueue_detach_unused_buf(channel->vq)) != NULL) {
        kfree(buf);
    }
}

void channel_send(struct virt_channel *channel, void *message) {
    struct virtqueue *vq = channel->vq;
    struct scatterlist sg;

	sg_init_one(&sg, message, sizeof(message));
	if (virtqueue_add_outbuf(vq, &sg, 1, channel, GFP_KERNEL) < 0)
        BUG();
	virtqueue_kick(vq);
};

/* will sleep until receive ack */
void channel_send_and_wait_ack(struct virt_channel *channel, void *message) {
    unsigned int len;
    channel_send(channel, message);
    wait_event(*channel->ack, virtqueue_get_buf(channel->vq, &len));
};

// *** Update Stats ***
#define pages_to_bytes(x) ((u64)(x) << PAGE_SHIFT)

static void update_stats(struct virtio_balloon *vb)
{
	struct sysinfo i;
	si_meminfo(&i);

    vb->stats[0] = pages_to_bytes(i.freeram);
    vb->stats[1] = pages_to_bytes(i.totalram);

    printk(KERN_WARNING "mem_free %lu, mem_total %lu", vb->stats[0], vb->stats[1]);
    channel_send(vb->stats_channel, vb->stats);
}
// *** End Update Stats ***


static unsigned long *pages_to_pfn_array(struct list_head *head, size_t len) {
    size_t i = 0;
    unsigned long pfns[len];
    struct page *page, *tmp;

    list_for_each_entry_safe(page, tmp, head, lru) {
		list_del(&page->lru);
		pfns[i++] = page_to_pfn(page);
	}

    return pfns;
}

// *** Balloon Func ***

static void inflate_balloon(struct virtio_balloon *vb){
    if (mutex_is_locked( &(vb->page_mutex) )) return;

    struct list_head pages;
    INIT_LIST_HEAD(&pages);
    size_t num_enqueued = 0;

    mutex_lock(&vb->page_mutex);
    unsigned int i;
    for (i=0 ; i<VIRTIO_BALLOON_PAGES_PER_32MB ; i++) {
        struct page *balloon_page = balloon_page_alloc();
        if (!balloon_page) {
            msleep(200);
			break;
        }
        balloon_page_enqueue(vb->balloon_dev_info, balloon_page);
        list_add(&balloon_page->lru, &pages);
        num_enqueued++;
    }

    if (num_enqueued) {
        channel_send_and_wait_ack(
            vb->inflate_channel,
            pages_to_pfn_array(&pages, num_enqueued)
        );
        vb->num_pages += num_enqueued;
    }
    mutex_unlock(&vb->page_mutex);
}

static void deflate_balloon(struct virtio_balloon *vb){
    if (!vb->num_pages) return;

    mutex_lock(&vb->page_mutex);
    struct list_head pages;
    INIT_LIST_HEAD(&pages);
    size_t num_dequeued = balloon_page_list_dequeue(
        vb->balloon_dev_info,
        &pages,
        VIRTIO_BALLOON_PAGES_PER_32MB
    );

    if (num_dequeued) {
        channel_send_and_wait_ack(
            vb->deflate_channel,
            pages_to_pfn_array(&pages, num_dequeued)
        );
        vb->num_pages -= num_dequeued;
    }
    mutex_unlock(&vb->page_mutex);
}

static int ballooning(void *data)
{
	struct virtio_balloon *vb = data;

	while (!kthread_should_stop()) {
        update_stats(vb);

        if (vb->stats[0] * 100 < vb->stats[1] * 70) {
            inflate_balloon(vb);
        } 
        if (vb->stats[0] * 100 < vb->stats[1] * 85) {
            deflate_balloon(vb);
        }

        msleep(10000);
	}
	return 0;
}
// *** End Balloon Func ***

// ******************** End Utils ********************


static int virtio_balloon_probe(struct virtio_device *vdev)
{
    int err = -1;
    struct virtio_balloon *vb = NULL;

    if (!(vb = kzalloc(sizeof(struct virtio_balloon), GFP_KERNEL)))
        return -ENOMEM;
    
    if (!(vb->balloon_dev_info = kzalloc(sizeof(struct balloon_dev_info), GFP_KERNEL))) {
        err = -ENOMEM;
        goto out_free_vb;
    }

    balloon_devinfo_init(vb->balloon_dev_info);

    if (
           !(vb->stats_channel =   create_virt_channel(vdev, "stats_channel"))
        || !(vb->inflate_channel = create_virt_channel(vdev, "inflate_channel"))
        || !(vb->deflate_channel = create_virt_channel(vdev, "deflate_channel"))
    ) goto out_del_vqs;

    vb->thread = kthread_run(ballooning, vb, "ballooning");
	if (IS_ERR(vb->thread)) {
		err = PTR_ERR(vb->thread);
		goto out_del_vqs;
	}

    mutex_init(&(vb->page_mutex));
    vb->num_pages = 0;
    vb->vdev = vdev;
    vdev->priv = vb;

    /* from this point on, the vdev can notify and get callbacks */
    virtio_device_ready(vdev);

    return 0;

out_del_vqs:
	vdev->config->del_vqs(vdev);
out_free_dev_info:
    kfree(vb->balloon_dev_info);
out_free_vb:
    kfree(vb);
out:
    return err;
}


static void virtio_balloon_remove(struct virtio_device *vdev)
{
    printk(KERN_WARNING"driver in exit");
    struct virtio_balloon *vb = vdev->priv;

    /* stop all ballooning thread */
    kthread_stop(vb->thread);
    /* free all pages left in the balloon */
    while (vb->num_pages)
		deflate_balloon(vb);
    /* detach unused buffers */
    free_channel_buf(vb->stats_channel);
    free_channel_buf(vb->inflate_channel);
    free_channel_buf(vb->deflate_channel);
    virtio_break_device(vdev);
    vdev->config->del_vqs(vdev);
    kfree(vb->balloon_dev_info);
    kfree(vb);
}


static const struct virtio_device_id id_table[] = {
    { VIRTIO_ID_BALLOON, VIRTIO_DEV_ANY_ID },
    { 0 },
};


static struct virtio_driver virtio_balloon_driver = {
    .driver.name =    "VirtIO Balloon Driver",
    .driver.owner =   THIS_MODULE,
    .id_table =       id_table,
    .probe =          virtio_balloon_probe, 
    .remove =         virtio_balloon_remove
};


module_virtio_driver(virtio_balloon_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("VirtIO Balloon Driver");
MODULE_LICENSE("GPL");