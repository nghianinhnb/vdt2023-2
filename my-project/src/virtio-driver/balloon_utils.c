#include <linux/virtio_balloon.h>
#include <linux/balloon_compaction.h>

#include <linux/balloon_utils.h>

//dev
#include <balloon_utils.h>


static struct page *balloon_pfn_to_page(u32 pfn)
{
	BUG_ON(pfn % VIRTIO_BALLOON_PAGES_PER_PAGE);
	return pfn_to_page(pfn / VIRTIO_BALLOON_PAGES_PER_PAGE);
}


static void add_page_to_balloon_pfns(u32 pfns[], struct page *page)
{
	BUILD_BUG_ON(PAGE_SHIFT < VIRTIO_BALLOON_PFN_SHIFT);

	unsigned long pfn = page_to_pfn(page);
	u32 start_balloon_pfn = pfn * VIRTIO_BALLOON_PAGES_PER_PAGE;

	for (unsigned int i = 0; i < VIRTIO_BALLOON_PAGES_PER_PAGE; i++)
		pfns[i] = start_balloon_pfn + i;
}
