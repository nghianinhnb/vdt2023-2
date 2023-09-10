#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/virtio/virtio.h"
#include "hw/mem/pc-dimm.h"
#include "hw/qdev-properties.h"
#include "hw/boards.h"
#include "sysemu/balloon.h"
#include "hw/virtio/virtio-balloon.h"
#include "exec/address-spaces.h"
#include "qapi/error.h"
#include "qapi/qapi-events-machine.h"
#include "qapi/visitor.h"
#include "trace.h"
#include "qemu/error-report.h"

#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-access.h"

#define BALLOON_PAGE_SIZE  (1 << VIRTIO_BALLOON_PFN_SHIFT)

#define LINUX_MEMCG_DEF_PATH "/sys/fs/cgroup/memory"
#define AUTO_BALLOON_NR_PAGES ((32 * 1024 * 1024) >> VIRTIO_BALLOON_PFN_SHIFT)


static void balloon_deflate_page(VirtIOBalloon *balloon,
                                 MemoryRegion *mr, hwaddr mr_offset)
{
    void *addr = memory_region_get_ram_ptr(mr) + mr_offset;
    ram_addr_t rb_offset;
    RAMBlock *rb;
    size_t rb_page_size;
    void *host_addr;
    int ret;

    rb = qemu_ram_block_from_host(addr, false, &rb_offset);
    rb_page_size = qemu_ram_pagesize(rb);

    host_addr = (void *)((uintptr_t)addr & ~(rb_page_size - 1));

    ret = qemu_madvise(host_addr, rb_page_size, QEMU_MADV_WILLNEED);
    if (ret != 0) {
        warn_report("Couldn't MADV_WILLNEED on balloon deflate: %s",
                    strerror(errno));
    }
}

static void balloon_inflate_page(
    VirtIOBalloon *balloon, MemoryRegion *mr, hwaddr mr_offset)
{
    void *addr = memory_region_get_ram_ptr(mr) + mr_offset;
    ram_addr_t rb_offset;
    RAMBlock *rb;
    size_t rb_page_size;

    rb = qemu_ram_block_from_host(addr, false, &rb_offset);
    rb_page_size = qemu_ram_pagesize(rb);

    ram_block_discard_range(rb, rb_offset, rb_page_size);
}

static void virtio_balloon_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(vdev);
    VirtQueueElement *elem;
    MemoryRegionSection section;

    fprintf(stdout, "page receive");

    for (;;) {
        size_t offset = 0;
        uint32_t pfn;

        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem)  break;

        while (iov_to_buf(elem->out_sg, elem->out_num, offset, &pfn, 4) == 4) {
            unsigned int p = virtio_ldl_p(vdev, &pfn);
            hwaddr pa;

            pa = (hwaddr) p << VIRTIO_BALLOON_PFN_SHIFT;
            offset += 4;

            section = memory_region_find(get_system_memory(), pa,
                                         BALLOON_PAGE_SIZE);
            if (!section.mr) continue;

            if (!memory_region_is_ram(section.mr) ||
                memory_region_is_rom(section.mr) ||
                memory_region_is_romd(section.mr)) {
                memory_region_unref(section.mr);
                continue;
            }

            if (vq == s->ivq) {
                balloon_inflate_page(s, section.mr,
                                        section.offset_within_region);
            } else if (vq == s->dvq) {
                balloon_deflate_page(s, section.mr, section.offset_within_region);
            } else {
                g_assert_not_reached();
            }

            memory_region_unref(section.mr);
        }

        virtqueue_push(vq, elem, 0);
        virtio_notify(vdev, vq);
        g_free(elem);
    }
}

static void virtio_balloon_receive_stats(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(vdev);
    VirtQueueElement *elem;
    uint64_t stat[2];
    size_t offset = 0;

    fprintf(stdout, "stat receive");

    elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
    if (!elem) return;

    if (s->stats_vq_elem != NULL) {
        g_free(s->stats_vq_elem);
    }

    s->stats_vq_elem = elem;
    while (iov_to_buf(elem->out_sg, elem->out_num, offset, &stat, sizeof(stat))
           == sizeof(stat)) {
        uint64_t memfree = virtio_tswap64(vdev, stat[0]);
        uint64_t memtotal = virtio_tswap64(vdev, stat[1]);
        offset += sizeof(stat);
        fprintf(stdout, "mem_free %lu, mem_total %lu", memfree, memtotal);
    }
}

static void virtio_balloon_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOBalloon *s = VIRTIO_BALLOON(dev);

    fprintf(stdout, "balloon device realize");

    virtio_init(vdev, "virtio-balloon", VIRTIO_ID_BALLOON, 0);

    s->ivq = virtio_add_queue(vdev, 128, virtio_balloon_handle_output);
    s->dvq = virtio_add_queue(vdev, 128, virtio_balloon_handle_output);
    s->svq = virtio_add_queue(vdev, 128, virtio_balloon_receive_stats);
}

static void virtio_balloon_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOBalloon *s = VIRTIO_BALLOON(dev);

    virtio_delete_queue(s->ivq);
    virtio_delete_queue(s->dvq);
    virtio_delete_queue(s->svq);
    virtio_cleanup(vdev);
}

static Property virtio_balloon_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_balloon_instance_init(Object *obj)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(obj);
}

static void virtio_balloon_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, virtio_balloon_properties);
    vdc->realize = virtio_balloon_device_realize;
    vdc->unrealize = virtio_balloon_device_unrealize;

    fprintf(stdout, "balloon device class init");
}

static const TypeInfo virtio_balloon_info = {
    .name = TYPE_VIRTIO_BALLOON,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOBalloon),
    .instance_init = virtio_balloon_instance_init,
    .class_init = virtio_balloon_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_balloon_info);
}

type_init(virtio_register_types)
