#include <linux/virtio_balloon.h>
#include <linux/balloon_compaction.h>

#define VIRTIO_BALLOON_PAGES_PER_PAGE (unsigned)(PAGE_SIZE >> VIRTIO_BALLOON_PFN_SHIFT)
#define VIRTIO_BALLOON_ARRAY_PFNS_MAX 256


static struct page *balloon_pfn_to_page(u32 pfn)

static void add_page_to_balloon_pfns(u32 pfns[], struct page *page)
