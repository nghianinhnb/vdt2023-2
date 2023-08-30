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


/**
 * The struct virtio_balloon represents a balloon device in the VirtIO framework.
 * 
 * @device: The virtual device that the virtio_balloon is associated with.
 * @message_virtqueue: A virtqueue is used for sending and receiving messages 
 * between the host and guest in a virtualized environment.
 * 
 * @guest_pressure: Represents the pressure from the guest on the device
 */
struct virtio_balloon 
{
    struct virtio_device *device;
    struct virtqueue *message_virtqueue;

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
	// wake_up(&balloon->config_change);
}


static void virtio_balloon_recv_cb(struct virtqueue *vq)
{
    struct virtio_balloon *balloon = vq->device->priv;
    char *buf;
    unsigned int len;

    while ((buf = virtqueue_get_buf(balloon->vq, &len)) != NULL) {
            /* process the received data */
    }
}


static int virtio_balloon_probe(struct virtio_device *device)
{
    struct virtio_balloon *balloon = NULL;

    /* initialize device data */
    balloon = kzalloc(sizeof(struct virtio_balloon), GFP_KERNEL);
    if (!balloon)
            return -ENOMEM;

    atomic_set(&balloon->guest_pressure, 0);
	/* end init device data */

    /* register virtqueues */
    balloon->vq = virtio_find_single_vq(device, virtio_balloon_recv_cb, "input");
    if (IS_ERR(balloon->vq)) {
            kfree(balloon);
            return PTR_ERR(balloon->vq);

    }
    device->priv = balloon;

    /* from this point on, the device can notify and get callbacks */
    virtio_device_ready(device);

    return 0;
}


static void virtio_balloon_remove(struct virtio_device *device)
{
    struct virtio_balloon *balloon = device->priv;

    /*
        * disable vq interrupts: equivalent to
        * device->config->reset(device)
        */
    virtio_reset_device(device);

    /* detach unused buffers */
    while ((buf = virtqueue_detach_unused_buf(balloon->vq)) != NULL) {
            kfree(buf);
    }

    /* remove virtqueues */
    device->config->del_vqs(device);

    kfree(balloon);
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