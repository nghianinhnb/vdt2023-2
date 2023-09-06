#include <linux/virtio_config.h>
#include <virt_channel.h>

struct virt_channel *create_virt_channel(struct virtio_device *vdev, char *name) {
    struct virt_channel *channel;

    if (!(channel = kzalloc(sizeof(struct virt_channel), GFP_KERNEL))) goto out;

    channel->vq = virtio_find_single_vq(vdev, callback, name);
    channel->vq->priv = channel;

    if (!(channel->ack = kzalloc(sizeof(wait_queue_head_t), GFP_KERNEL)))
        goto out;

    init_waitqueue_head(channel->ack);

    return channel;
out:
    return -ENOMEM;
};

static void channel_send(struct virt_channel *channel, void *message) {
    struct virtqueue *vq = channel->vq;
    struct scatterlist *sg;
	unsigned int len;

	sg_init_one(sg, message, sizeof(message));
	if (virtqueue_add_outbuf(vq, sg, 1, channel, GFP_KERNEL) < 0)
        BUG();
	virtqueue_kick(vq);
};

void channel_send_and_wait_ack(struct virt_channel *channel, void *message) {
    channel_send(channel, message);
    if (channel->ack) {
        unsigned int len;
        wait_event(channel->ack, virtqueue_get_buf(channel->vq, &len));
    }
};

static inline void ack(struct virt_channel *channel) {
    wake_up(channel->ack);
};

static void callback(struct virtqueue *vq) {
    struct virt_channel *channel = vq->priv;
    ack(channel);
}
