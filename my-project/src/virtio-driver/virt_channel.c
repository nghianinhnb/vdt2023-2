#include <linux/err.h>
#include <virt_channel.h>
#include <linux/virtio_config.h>


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
    struct scatterlist *sg;

	sg_init_one(sg, message, sizeof(message));
	if (virtqueue_add_outbuf(vq, sg, 1, channel, GFP_KERNEL) < 0)
        BUG();
	virtqueue_kick(vq);
};

/* will sleep until receive ack */
void channel_send_and_wait_ack(struct virt_channel *channel, void *message) {
    channel_send(channel, message);
    unsigned int len;
    wait_event(channel->ack, virtqueue_get_buf(channel->vq, &len));
};

static inline void ack(struct virt_channel *channel) {
    wake_up(channel->ack);
};

static void callback(struct virtqueue *vq) {
    struct virt_channel *channel = vq->priv;
    ack(channel);
}
