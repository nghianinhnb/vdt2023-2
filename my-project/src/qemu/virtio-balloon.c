/*
 * Virtio Balloon Device
 *
 * Copyright IBM, Corp. 2008
 * Copyright (C) 2011 Red Hat, Inc.
 * Copyright (C) 2011 Amit Shah <amit.shah@redhat.com>
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "qemu/iov.h"
#include "qemu/timer.h"
#include "qemu-common.h"
#include "hw/virtio/virtio.h"
#include "hw/i386/pc.h"
#include "cpu.h"
#include "sysemu/balloon.h"
#include "hw/virtio/virtio-balloon.h"
#include "sysemu/kvm.h"
#include "exec/address-spaces.h"
#include "qapi/visitor.h"

#if defined(__linux__)
#include <sys/mman.h>
#endif

#include "hw/virtio/virtio-bus.h"

#define LINUX_MEMCG_DEF_PATH "/sys/fs/cgroup/memory"
#define AUTO_BALLOON_NR_PAGES ((32 * 1024 * 1024) >> VIRTIO_BALLOON_PFN_SHIFT)
#define AUTO_BALLOON_PRESSURE_PERIOD 60

void virtio_balloon_set_conf(DeviceState *dev, const VirtIOBalloonConf *bconf)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(dev);
    memcpy(&(s->bconf), bconf, sizeof(struct VirtIOBalloonConf));
}

static bool auto_balloon_enabled_cmdline(const VirtIOBalloon *s)
{
    return s->bconf.auto_balloon_enabled;
}

static bool guest_in_pressure(const VirtIOBalloon *s)
{
    time_t t = s->autob_last_guest_pressure;
    return difftime(time(NULL), t) <= AUTO_BALLOON_PRESSURE_PERIOD;
}

static void inflate_guest(VirtIOBalloon *s)
{
    if (guest_in_pressure(s)) {
        s->nr_events_inflate_skipped++;
        return;
    }

    s->num_pages += AUTO_BALLOON_NR_PAGES;
    virtio_notify_config(VIRTIO_DEVICE(s));

    s->nr_events_inflate_handled++;
}

static void deflate_guest(VirtIOBalloon *s)
{
    if (!s->autob_cur_size) {
        s->nr_events_deflate_skipped++;
        return;
    }

    s->num_pages -= AUTO_BALLOON_NR_PAGES;
    virtio_notify_config(VIRTIO_DEVICE(s));
    s->nr_events_deflate_handled++;
}

static void virtio_balloon_handle_host_pressure(EventNotifier *ev)
{
    VirtIOBalloon *s = container_of(ev, VirtIOBalloon, event);

    if (!event_notifier_test_and_clear(ev)) {
        fprintf(stderr, "virtio-balloon: failed to drain the notify pipe\n");
        return;
    }

    inflate_guest(s);
}

static void register_vmpressure(int cfd, int efd, int lfd, Error **errp)
{
    char *p;
    ssize_t ret;

    p = g_strdup_printf("%d %d low",  efd, lfd);
    ret = write(cfd, p, strlen(p));
    if (ret < 0) {
        error_setg_errno(errp, errno, "failed to write to control fd: %d", cfd);
    } else {
        g_assert(ret == strlen(p)); /* XXX: this should be always true, right? */
    }

    g_free(p);
}

static int open_file_in_dir(const char *dir_path, const char *file, mode_t mode,
                            Error **errp)
{
    char *p;
    int fd;

    p = g_strjoin("/", dir_path, file, NULL);
    fd = qemu_open(p, mode);
    if (fd < 0) {
        error_setg_errno(errp, errno, "can't open '%s'", p);
    }

    g_free(p);
    return fd;
}

static void automatic_balloon_init(VirtIOBalloon *s, const char *memcg_path,
                                   Error **errp)
{
    Error *local_err = NULL;
    int ret;

    if (!memcg_path) {
        memcg_path = LINUX_MEMCG_DEF_PATH;
    }

    s->lfd = open_file_in_dir(memcg_path, "memory.pressure_level", O_RDONLY,
                              &local_err);
    if (local_err) {
        goto out;
    }

    s->cfd = open_file_in_dir(memcg_path, "cgroup.event_control", O_WRONLY,
                              &local_err);
    if (local_err) {
        close(s->lfd);
        goto out;
    }

    ret = event_notifier_init(&s->event, false);
    if (ret < 0) {
        error_setg_errno(&local_err, -ret, "failed to create event notifier");
        goto out_err;
    }

    s->autob_last_guest_pressure = time(NULL) - (AUTO_BALLOON_PRESSURE_PERIOD+1);
    event_notifier_set_handler(&s->event, virtio_balloon_handle_host_pressure);

    register_vmpressure(s->cfd, event_notifier_get_fd(&s->event), s->lfd,
                            &local_err);
    if (local_err) {
        event_notifier_cleanup(&s->event);
        goto out_err;
    }

    fprintf(stderr, "-> registered\n");
    return;

out_err:
    close(s->lfd);
    close(s->cfd);
out:
    error_propagate(errp, local_err);
}

/* auto-ballooning stats, for debug only */
#define PAGES_TO_KB(x)   ((x * 4096) / 1024)
#define PAGES_TO_MB(x)   ((x * 4096) / (1024 * 1024))
#define BYTES_TO_MB(x)   (x / (1024 * 1024))

static void print_pages_info(const char *desc, unsigned int nr_pages)
{
    fprintf(stderr, "%s: %d (%dKB/%dMB)\n", desc, nr_pages,
            PAGES_TO_KB(nr_pages), PAGES_TO_MB(nr_pages));
}

static void auto_balloon_stats_print(const BalloonInfo *info,const VirtIOBalloon *dev)
{
    /* to be dropped */
    fprintf(stderr, "\n");
    print_pages_info("assigned memory", ram_size >> VIRTIO_BALLOON_PFN_SHIFT);
    print_pages_info("current memory",info->actual >> VIRTIO_BALLOON_PFN_SHIFT);
    print_pages_info("balloon actual", dev->actual);
    print_pages_info("balloon size", dev->autob_cur_size);
    fprintf(stderr, "guest in pressure %d (counting: %ds)\n",
            guest_in_pressure(dev),
            (int) difftime(time(NULL), dev->autob_last_guest_pressure));
    fprintf(stderr, "\nauto inflate\n");
    print_pages_info("pages inflated", dev->nr_pages_inflated);
    fprintf(stderr, "  events handled: %d\n",dev->nr_events_inflate_handled);
    fprintf(stderr, "  skipped (g. pressure): %d\n", dev->nr_events_inflate_skipped);
    fprintf(stderr, "  inflates stopped: %d\n", dev->nr_inflate_stopped);
    fprintf(stderr, "\nauto deflate\n");
    print_pages_info("pages deflated", dev->nr_pages_deflated);
    fprintf(stderr, "  deflates handled: %d\n",dev->nr_events_deflate_handled);
    fprintf(stderr, "  skipped (b. empty): %d\n",dev->nr_events_deflate_skipped);
    fprintf(stderr, "\n\n\n");
}

static void balloon_page(void *addr, int deflate)
{
#if defined(__linux__)
    if (!kvm_enabled() || kvm_has_sync_mmu())
        qemu_madvise(addr, TARGET_PAGE_SIZE,
                deflate ? QEMU_MADV_WILLNEED : QEMU_MADV_DONTNEED);
#endif
}

static const char *balloon_stat_names[] = {
   [VIRTIO_BALLOON_S_SWAP_IN] = "stat-swap-in",
   [VIRTIO_BALLOON_S_SWAP_OUT] = "stat-swap-out",
   [VIRTIO_BALLOON_S_MAJFLT] = "stat-major-faults",
   [VIRTIO_BALLOON_S_MINFLT] = "stat-minor-faults",
   [VIRTIO_BALLOON_S_MEMFREE] = "stat-free-memory",
   [VIRTIO_BALLOON_S_MEMTOT] = "stat-total-memory",
   [VIRTIO_BALLOON_S_NR] = NULL
};

/*
 * reset_stats - Mark all items in the stats array as unset
 *
 * This function needs to be called at device initialization and before
 * updating to a set of newly-generated stats.  This will ensure that no
 * stale values stick around in case the guest reports a subset of the supported
 * statistics.
 */
static inline void reset_stats(VirtIOBalloon *dev)
{
    int i;
    for (i = 0; i < VIRTIO_BALLOON_S_NR; dev->stats[i++] = -1);
}

static bool balloon_stats_supported(const VirtIOBalloon *s)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    return vdev->guest_features & (1 << VIRTIO_BALLOON_F_STATS_VQ);
}

static bool balloon_stats_enabled(const VirtIOBalloon *s)
{
    return s->stats_poll_interval > 0;
}

static void balloon_stats_destroy_timer(VirtIOBalloon *s)
{
    if (balloon_stats_enabled(s)) {
        timer_del(s->stats_timer);
        timer_free(s->stats_timer);
        s->stats_timer = NULL;
        s->stats_poll_interval = 0;
    }
}

static void balloon_stats_change_timer(VirtIOBalloon *s, int secs)
{
    timer_mod(s->stats_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + secs * 1000);
}

static void balloon_stats_poll_cb(void *opaque)
{
    VirtIOBalloon *s = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    if (!balloon_stats_supported(s)) {
        /* re-schedule */
        balloon_stats_change_timer(s, s->stats_poll_interval);
        return;
    }

    virtqueue_push(s->svq, &s->stats_vq_elem, s->stats_vq_offset);
    virtio_notify(vdev, s->svq);
}

static void balloon_stats_get_all(Object *obj, struct Visitor *v,
                                  void *opaque, const char *name, Error **errp)
{
    VirtIOBalloon *s = opaque;
    int i;

    if (!s->stats_last_update) {
        error_setg(errp, "guest hasn't updated any stats yet");
        return;
    }

    visit_start_struct(v, NULL, "guest-stats", name, 0, errp);
    visit_type_int(v, &s->stats_last_update, "last-update", errp);

    visit_start_struct(v, NULL, NULL, "stats", 0, errp);
    for (i = 0; i < VIRTIO_BALLOON_S_NR; i++) {
        visit_type_int64(v, (int64_t *) &s->stats[i], balloon_stat_names[i],
                         errp);
    }
    visit_end_struct(v, errp);

    visit_end_struct(v, errp);
}

static void balloon_stats_get_poll_interval(Object *obj, struct Visitor *v,
                                            void *opaque, const char *name,
                                            Error **errp)
{
    VirtIOBalloon *s = opaque;
    visit_type_int(v, &s->stats_poll_interval, name, errp);
}

static void balloon_stats_set_poll_interval(Object *obj, struct Visitor *v,
                                            void *opaque, const char *name,
                                            Error **errp)
{
    VirtIOBalloon *s = opaque;
    int64_t value;

    visit_type_int(v, &value, name, errp);
    if (error_is_set(errp)) {
        return;
    }

    if (value < 0) {
        error_setg(errp, "timer value must be greater than zero");
        return;
    }

    if (value == s->stats_poll_interval) {
        return;
    }

    if (value == 0) {
        /* timer=0 disables the timer */
        balloon_stats_destroy_timer(s);
        return;
    }

    if (balloon_stats_enabled(s)) {
        /* timer interval change */
        s->stats_poll_interval = value;
        balloon_stats_change_timer(s, value);
        return;
    }

    /* create a new timer */
    g_assert(s->stats_timer == NULL);
    s->stats_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, balloon_stats_poll_cb, s);
    s->stats_poll_interval = value;
    balloon_stats_change_timer(s, 0);
}

static void virtio_balloon_handle_msg(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBalloon *dev = VIRTIO_BALLOON(vdev);
    VirtQueueElement elem;

    while (virtqueue_pop(vq, &elem)) {
        size_t offset = 0;
        uint32_t msg;

        while (iov_to_buf(elem.out_sg, elem.out_num, offset, &msg, 4) == 4) {
            offset += 4;
            msg = ldl_p(&msg);

            if (msg == VIRTIO_BALLOON_MSG_PRESSURE) {
                dev->autob_last_guest_pressure = time(NULL);
                if (dev->num_pages > dev->autob_cur_size) {
                    /* cancel on-going inflation */
                    dev->num_pages = dev->autob_cur_size;
                    dev->nr_inflate_stopped++;
                } else {
                    deflate_guest(dev);
                }
            }
        }
        virtqueue_push(vq, &elem, offset);
        virtio_notify(vdev, vq);
    }
}

static void virtio_balloon_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(vdev);
    VirtQueueElement elem;
    MemoryRegionSection section;

    while (virtqueue_pop(vq, &elem)) {
        size_t offset = 0;
        uint32_t pfn;

        while (iov_to_buf(elem.out_sg, elem.out_num, offset, &pfn, 4) == 4) {
            ram_addr_t pa;
            ram_addr_t addr;

            pa = (ram_addr_t)ldl_p(&pfn) << VIRTIO_BALLOON_PFN_SHIFT;
            offset += 4;

            /* FIXME: remove get_system_memory(), but how? */
            section = memory_region_find(get_system_memory(), pa, 1);
            if (!int128_nz(section.size) || !memory_region_is_ram(section.mr))
                continue;

            /* Using memory_region_get_ram_ptr is bending the rules a bit, but
               should be OK because we only want a single page.  */
            addr = section.offset_within_region;
            balloon_page(memory_region_get_ram_ptr(section.mr) + addr,
                         !!(vq == s->dvq));
            memory_region_unref(section.mr);

            if (vq == s->ivq) {
                s->nr_pages_inflated++;
                s->autob_cur_size++;
            } else {
                s->nr_pages_deflated++;
                s->autob_cur_size--;
            }
        }

        virtqueue_push(vq, &elem, offset);
        virtio_notify(vdev, vq);
    }
}

static void virtio_balloon_receive_stats(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(vdev);
    VirtQueueElement *elem = &s->stats_vq_elem;
    VirtIOBalloonStat stat;
    size_t offset = 0;
    qemu_timeval tv;

    if (!virtqueue_pop(vq, elem)) {
        goto out;
    }

    /* Initialize the stats to get rid of any stale values.  This is only
     * needed to handle the case where a guest supports fewer stats than it
     * used to (ie. it has booted into an old kernel).
     */
    reset_stats(s);

    while (iov_to_buf(elem->out_sg, elem->out_num, offset, &stat, sizeof(stat))
           == sizeof(stat)) {
        uint16_t tag = tswap16(stat.tag);
        uint64_t val = tswap64(stat.val);

        offset += sizeof(stat);
        if (tag < VIRTIO_BALLOON_S_NR)
            s->stats[tag] = val;
    }
    s->stats_vq_offset = offset;

    if (qemu_gettimeofday(&tv) < 0) {
        fprintf(stderr, "warning: %s: failed to get time of day\n", __func__);
        goto out;
    }

    s->stats_last_update = tv.tv_sec;

out:
    if (balloon_stats_enabled(s)) {
        balloon_stats_change_timer(s, s->stats_poll_interval);
    }
}

static void virtio_balloon_get_config(VirtIODevice *vdev, uint8_t *config_data)
{
    VirtIOBalloon *dev = VIRTIO_BALLOON(vdev);
    struct virtio_balloon_config config;

    config.num_pages = cpu_to_le32(dev->num_pages);
    config.actual = cpu_to_le32(dev->actual);

    memcpy(config_data, &config, 8);
}

static void virtio_balloon_set_config(VirtIODevice *vdev,
                                      const uint8_t *config_data)
{
    VirtIOBalloon *dev = VIRTIO_BALLOON(vdev);
    struct virtio_balloon_config config;
    uint32_t oldactual = dev->actual;
    memcpy(&config, config_data, 8);
    dev->actual = le32_to_cpu(config.actual);
    if (dev->actual != oldactual) {
        qemu_balloon_changed(ram_size -
                       ((ram_addr_t) dev->actual << VIRTIO_BALLOON_PFN_SHIFT));
    }
}

static uint32_t virtio_balloon_get_features(VirtIODevice *vdev, uint32_t f)
{
    f |= (1 << VIRTIO_BALLOON_F_STATS_VQ);
    f |= (1 << VIRTIO_BALLOON_F_MESSAGE_VQ);

    return f;
}

static void virtio_balloon_stat(void *opaque, BalloonInfo *info)
{
    VirtIOBalloon *dev = opaque;
    info->actual = ram_size - ((uint64_t) dev->actual <<
                               VIRTIO_BALLOON_PFN_SHIFT);

    auto_balloon_stats_print(info, dev);
}

static void virtio_balloon_to_target(void *opaque, ram_addr_t target)
{
    VirtIOBalloon *dev = VIRTIO_BALLOON(opaque);
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);

    if (target > ram_size) {
        target = ram_size;
    }
    if (target) {
        dev->num_pages = (ram_size - target) >> VIRTIO_BALLOON_PFN_SHIFT;
        virtio_notify_config(vdev);
    }
}

static void virtio_balloon_save(QEMUFile *f, void *opaque)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(opaque);
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    virtio_save(vdev, f);

    qemu_put_be32(f, s->num_pages);
    qemu_put_be32(f, s->actual);
}

static int virtio_balloon_load(QEMUFile *f, void *opaque, int version_id)
{
    VirtIOBalloon *s = VIRTIO_BALLOON(opaque);
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    int ret;

    if (version_id != 1)
        return -EINVAL;

    ret = virtio_load(vdev, f);
    if (ret) {
        return ret;
    }

    s->num_pages = qemu_get_be32(f);
    s->actual = qemu_get_be32(f);
    return 0;
}

static void virtio_balloon_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOBalloon *s = VIRTIO_BALLOON(dev);
    Error *local_err = NULL;
    int ret;

    virtio_init(vdev, "virtio-balloon", VIRTIO_ID_BALLOON, 8);

    if (auto_balloon_enabled_cmdline(s)) {
        automatic_balloon_init(s, NULL /* default root memcg path */, &local_err);
        if (local_err) {
            virtio_cleanup(VIRTIO_DEVICE(s));
            error_propagate(errp, local_err);
            return;
        }
    }

    ret = qemu_add_balloon_handler(virtio_balloon_to_target,
                                   virtio_balloon_stat, s);

    if (ret < 0) {
        error_setg(errp, "Adding balloon handler failed");
        virtio_cleanup(vdev);
        return;
    }

    s->ivq = virtio_add_queue(vdev, 128, virtio_balloon_handle_output);
    s->dvq = virtio_add_queue(vdev, 128, virtio_balloon_handle_output);
    s->svq = virtio_add_queue(vdev, 128, virtio_balloon_receive_stats);
    s->mvq = virtio_add_queue(vdev, 128, virtio_balloon_handle_msg);

    register_savevm(dev, "virtio-balloon", -1, 1,
                    virtio_balloon_save, virtio_balloon_load, s);

    object_property_add(OBJECT(dev), "guest-stats", "guest statistics",
                        balloon_stats_get_all, NULL, NULL, s, NULL);

    object_property_add(OBJECT(dev), "guest-stats-polling-interval", "int",
                        balloon_stats_get_poll_interval,
                        balloon_stats_set_poll_interval,
                        NULL, s, NULL);
}

static void virtio_balloon_device_unrealize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOBalloon *s = VIRTIO_BALLOON(dev);

    balloon_stats_destroy_timer(s);
    qemu_remove_balloon_handler(s);
    unregister_savevm(dev, "virtio-balloon", s);
    virtio_cleanup(vdev);
}

static Property virtio_balloon_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_balloon_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    dc->props = virtio_balloon_properties;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = virtio_balloon_device_realize;
    vdc->unrealize = virtio_balloon_device_unrealize;
    vdc->get_config = virtio_balloon_get_config;
    vdc->set_config = virtio_balloon_set_config;
    vdc->get_features = virtio_balloon_get_features;
}

static const TypeInfo virtio_balloon_info = {
    .name = TYPE_VIRTIO_BALLOON,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOBalloon),
    .class_init = virtio_balloon_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_balloon_info);
}

type_init(virtio_register_types)