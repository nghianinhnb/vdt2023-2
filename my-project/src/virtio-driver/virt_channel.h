#ifndef VIRT_CHANNEL_H
#define VIRT_CHANNEL_H

#include <linux/wait.h>
#include <linux/virtio.h>

struct virt_channel {
    struct virtqueue *vq;
	wait_queue_head_t *ack;
};

struct virt_channel *create_virt_channel(struct virtio_device *vdev, char *name);
void channel_send_and_wait_ack(struct virt_channel* channel, void* message);

#endif // VIRT_CHANNEL_H