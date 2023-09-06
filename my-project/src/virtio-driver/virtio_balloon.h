#ifndef _LINUX_VIRTIO_BALLOON_H
#define _LINUX_VIRTIO_BALLOON_H

#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>

/* Size of a PFN in the balloon interface. */
#define VIRTIO_BALLOON_PFN_SHIFT 12

#define VIRTIO_BALLOON_S_MEMFREE  0   /* Total amount of free memory */
#define VIRTIO_BALLOON_S_MEMTOT   1   /* Total amount of memory */

#endif /* _LINUX_VIRTIO_BALLOON_H */