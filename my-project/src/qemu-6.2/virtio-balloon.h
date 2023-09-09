#ifndef QEMU_VIRTIO_BALLOON_H
#define QEMU_VIRTIO_BALLOON_H

#include "standard-headers/linux/virtio_balloon.h"
#include "hw/virtio/virtio.h"
#include "sysemu/iothread.h"
#include "qom/object.h"

#define VIRTIO_ID_BALLOON 5
#define VIRTIO_BALLOON_PFN_SHIFT 12
#define VIRTIO_BALLOON_S_MEMFREE  0   /* Total amount of free memory */
#define VIRTIO_BALLOON_S_MEMTOT   1   /* Total amount of memory */

#define TYPE_VIRTIO_BALLOON "virtio-balloon-device"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOBalloon, VIRTIO_BALLOON)

struct VirtIOBalloon {
    VirtIODevice parent_obj;
    VirtQueue *ivq, *dvq, *svq;
    uint64_t stat[2];
    VirtQueueElement *stats_vq_elem;
};

#endif