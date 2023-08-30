#include <linux/types.h>
#include <linux/virtio.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_balloon.h>
#include <linux/virtio_config.h>
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
    struct virtio_device *vdev;
    struct virtqueue *message_vq;

    /* Message virtqueue */
    atomic_t guest_pressure;
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


static void virtio_balloon_recv_cb(struct virtqueue *vq)
{
    struct virtio_balloon *vb = vq->vdev->priv;
    char *buf;
    unsigned int len;

    while ((buf = virtqueue_get_buf(vb->vq, &len)) != NULL) {
            /* process the received data */
    }
}


static int virtio_balloon_probe(struct virtio_device *vdev)
{
    struct virtio_balloon *vb = NULL;

    /* initialize device data */
    vb = kzalloc(sizeof(struct virtio_balloon), GFP_KERNEL);
    if (!vb)
            return -ENOMEM;

    atomic_set(&vb->guest_pressure, 0);

    /* the device has a single virtqueue */
    vb->vq = virtio_find_single_vq(vdev, virtio_balloon_recv_cb, "input");
    if (IS_ERR(vb->vq)) {
            kfree(vb);
            return PTR_ERR(vb->vq);

    }
    vdev->priv = vb;

    /* from this point on, the device can notify and get callbacks */
    virtio_device_ready(vdev);

    return 0;
}


static void virtio_balloon_remove(struct virtio_device *vdev)
{
    struct virtio_balloon *vb = vdev->priv;

    /*
        * disable vq interrupts: equivalent to
        * vdev->config->reset(vdev)
        */
    virtio_reset_device(vdev);

    /* detach unused buffers */
    while ((buf = virtqueue_detach_unused_buf(vb->vq)) != NULL) {
            kfree(buf);
    }

    /* remove virtqueues */
    vdev->config->del_vqs(vdev);

    kfree(vb);
}


static const struct virtio_device_id id_table[] = {
    { VIRTIO_ID_BALLOON, VIRTIO_DEV_ANY_ID },
	{ 0 },
};


static struct virtio_driver virtio_balloon_driver = {
    .driver.name =  KBUILD_MODNAME,
    .driver.owner = THIS_MODULE,
    .id_table =     id_table,
    .probe =        virtio_balloon_probe,
    .remove =       virtio_balloon_remove,
};


module_virtio_driver(virtio_balloon_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio balloon driver");
MODULE_LICENSE("GPL");