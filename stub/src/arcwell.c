// SPDX-License-Identifier: GPL-2.0
/* arcwell — the module. Serves stub/include/aw_uapi.h.
 *
 * Route (all of it verified from the target's own source and hardware,
 * see BACKLOG.md "The route" and results/M0_GATE_2026-09-15.txt):
 *
 *   userspace creates a host-visible VRAM BO on xe and exports it as a dma-buf,
 *   then hands us the fd. We:
 *     1. dma_buf_dynamic_attach() with allow_peer2peer = true  -- NOT
 *        dma_buf_attach(), which leaves peer2peer false and makes xe silently
 *        migrate the BO to system RAM (defect D3: a host bounce reported as
 *        success);
 *     2. RE-CHECK attach->peer2peer, because xe clears it silently when
 *        pci_p2pdma_distance() < 0 (xe_dma_buf.c:30-32). This check IS the
 *        AW_MAP_F_REQUIRE_P2P contract;
 *     3. pin + map the attachment. xe builds the sg with
 *        sg_set_page(sg, NULL, ...) (xe_ttm_vram_mgr.c:412), so the entries are
 *        PAGE-LESS and carry only sg_dma_address();
 *     4. recover the BAR2 physical address with iommu_iova_to_phys(), since the
 *        target has a translating AMD-Vi IOMMU;
 *     5. pci_p2pdma_add_resource() that range so ZONE_DEVICE P2P struct pages
 *        exist for it, then pfn_to_page() them;
 *     6. bio from those pages -> submit_bio_wait(). nvme.ko uses
 *        blk_rq_dma_map_iter_start/next and handles P2P natively.
 *
 * No step here can bounce: if a mapping cannot be done peer-to-peer the ioctl
 * FAILS rather than falling back. via_host_bounce counts those REFUSALS (see the
 * two increment sites in aw_map_buffer), so it is not "never incremented" -- it is
 * never incremented by a transfer that actually went through host memory, because
 * no such transfer exists.
 *
 * NOTE FOR CALLERS: the counter is MODULE-GLOBAL, not per-client. An absolute
 * reading says nothing about your run; compare it before and after.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/xarray.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pci-p2pdma.h>
#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/iommu.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/pm_runtime.h>
#include "aw_uapi.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("arcwell: NVMe -> Intel Arc VRAM direct storage");
MODULE_IMPORT_NS("DMA_BUF");

static char *arc_bdf = "";
module_param(arc_bdf, charp, 0444);
static char *nvme_bdf = "";
module_param(nvme_bdf, charp, 0444);
static char *bdev_path = "/dev/nvme0n1";
module_param(bdev_path, charp, 0444);

static struct pci_dev *g_arc, *g_nvme;
static struct file *g_bf;
static struct block_device *g_bdev;
static phys_addr_t g_bar_base;
static resource_size_t g_bar_len;
static int bar_idx = 2;
module_param(bar_idx, int, 0444);

/* Usable VRAM, in bytes, as xe reports it at boot:
 *   "VRAM[0]: ... usable size exclude stolen 0x3fa000000"
 * pci_resource_len() is NOT this. It is the BAR aperture, which is larger:
 *   A770  BAR2 16 GiB, usable 0x3fa000000 -> 96 MiB of stolen at the top
 *   B60   BAR2 32 GiB, VRAM only 24 GiB   -> 8 GiB of BAR is not memory at all
 * Carving past the usable limit registers ZONE_DEVICE pages over stolen or
 * absent memory and lets a DMA land there. Measured 2026-09-15: a 128 MiB
 * section carve at 0x7bf8000000 ran to 0x7c00000000, i.e. 96 MiB past the
 * A770's usable end of 0x7bfa000000. Set this from the boot log.
 * 0 = fall back to the BAR length, which is the unsafe legacy behaviour.
 */
static unsigned long long vram_usable;
module_param(vram_usable, ullong, 0444);
MODULE_PARM_DESC(vram_usable, "usable VRAM bytes from xe's boot log; 0 = BAR length (unsafe)");

/* Carve the whole usable VRAM once at load instead of growing on demand.
 *
 * A carve CANNOT be released: pci_p2pdma_add_resource() has no counterpart in
 * this kernel and the registration is devres-owned until the device is unbound.
 * Incremental carving therefore only ever grows, and in the limit it reaches the
 * whole of usable VRAM anyway. Paying that up front makes the cost deterministic
 * and removes the granule walk-down, the in-section conflict handling and the
 * -ENOSPC retries, and guarantees every BO xe can hand out is already covered.
 *
 * The cost is struct pages for the carved range: at 4 KiB pages and a 64-byte
 * struct page that is ~1.56% of VRAM in host RAM -- about 382 MiB for a 24 GiB
 * B60. That is housekeeping metadata, not a data buffer.
 */
static bool carve_all;
module_param(carve_all, bool, 0444);
MODULE_PARM_DESC(carve_all, "carve all usable VRAM at load (deterministic) instead of on demand");

/* FAULT INJECTION, for the cells only.
 *
 * via_host_bounce exists so a host bounce cannot hide. But with no way to reach
 * the detection path, every cell asserting via_host_bounce == 0 was reading a
 * field that is zero by construction -- decoration under HOUSE_RULES.md §5, and
 * it took a reviewer to notice.
 *
 * With this set, AW_IOC_MAP_BUFFER treats the attachment as though the exporter
 * had cleared peer2peer, exercising the detect-count-refuse path.
 *
 * WHY SIMULATE RATHER THAN REPRODUCE. The obvious injection -- attach with plain
 * dma_buf_attach() -- does not reach the check. Measured 2026-09-15: xe returns
 * -EOPNOTSUPP from dma_buf_attach() itself, because xe_dma_buf_attach() refuses a
 * non-peer2peer attach whose BO cannot migrate to system memory
 * (xe_dma_buf.c: "!attach->peer2peer && !xe_bo_can_migrate(bo, XE_PL_TT)"), and a
 * NEEDS_VISIBLE_VRAM BO cannot. So for the BO type arcwell requires, xe blocks
 * the bounce EARLIER than this module does.
 *
 * That is worth stating plainly: defect D3 as originally described -- plain
 * dma_buf_attach() silently migrating the buffer to host RAM -- applies to a BO
 * that CAN migrate. It cannot happen to a VRAM-only BO. The check below is
 * defence in depth against a future BO placement that can migrate, and this flag
 * is the only way to exercise it on this hardware.
 */
static bool debug_force_bounce;
module_param(debug_force_bounce, bool, 0644);
MODULE_PARM_DESC(debug_force_bounce, "TEST ONLY: attach without peer2peer so the bounce detector can be exercised");

static DEFINE_MUTEX(g_lock);
static LIST_HEAD(g_carves);
static u32 g_next_handle = 1;
static u64 g_carve_bytes;
static u32 g_carve_count;
static struct aw_ioc_stats g_stats;

struct aw_carve { phys_addr_t start; size_t len; struct list_head node; };

/* Completion accounting shared by the sync and async paths. Defined here
 * because struct aw_inflight embeds it by value. */
struct aw_batch {
	atomic_t pending;
	struct completion done;
	blk_status_t status;
};


/* Per-open state. Buffers belong to the file that registered them, not to the
 * module, so a client that exits or crashes cannot strand pinned VRAM. */
struct aw_client {
	/* xarray, NOT a list. aw_find_get() is called once per request inside the
	 * batch loop, so a list makes AW_IOC_READ_BATCH O(requests x live buffers).
	 * M4_API.md's streaming shape registers the whole expert set once at load,
	 * so "live buffers" is the expert count: at 2000 buffers and a 256-request
	 * batch that is 512,000 pointer chases per batch, milliseconds of
	 * bookkeeping against a 1.125 ms per-expert transfer. Keyed by handle. */
	struct xarray buffers;
	/* In-flight batches, keyed by id. Submitted but not yet collected. */
	struct xarray batches;
	u64 next_batch_id;
	struct mutex lock;	/* guards live/peak/next_batch_id */
	u32 live, peak;
};

/* One submitted-but-uncollected batch. Lives until AW_IOC_BATCH_WAIT collects it
 * or the fd closes. The bios point at &inf->ba, so it must NOT be freed while any
 * are in flight -- release drains before freeing. */
struct aw_inflight {
	u64 id;
	struct aw_batch ba;
	struct aw_buffer **refs;
	unsigned int nrefs, submitted, nbio;
	u64 bytes;
	/* Set while one thread owns the collection of this batch. Two threads
	 * calling AW_IOC_BATCH_WAIT on the same id used to both xa_load() the
	 * same pointer, both park on ba.done, and -- because complete() wakes
	 * exactly one -- the winner would free this allocation while the loser
	 * was still parked inside it. Claiming under cl->lock BEFORE waiting is
	 * what makes that unrepresentable: the loser never reaches the wait. */
	bool collecting;
};

struct aw_buffer {
	u32 handle;
	struct kref ref;
	/* Set if move_notify ever fires. The pages array was resolved once, at
	 * map time, and every bio is built from it; if the exporter moved the BO
	 * those addresses are stale and any further transfer would DMA into
	 * whatever now lives there. There is no way to recall bios already in
	 * flight, so the only honest response is to refuse everything after. */
	bool moved;
	struct dma_buf *dbuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct page **pages;
	unsigned int npages;
	size_t length;
};

/* This must not fire. dma_buf_pin() is taken at map time (see aw_map_buffer
 * step 3) and held for the buffer's whole life, and a pinned attachment is not
 * movable. If it fires anyway the pin contract is broken by the exporter, and
 * b->pages[] -- resolved once at map time and used to build every bio -- is
 * stale.
 *
 * pr_warn alone was not a response to that. It left a buffer whose pages point
 * at memory the GPU no longer owns still accepting transfers. Now the buffer is
 * poisoned: every later lookup refuses it. Bios ALREADY in flight cannot be
 * recalled, which is the part this cannot fix and does not pretend to -- hence
 * WARN, so the trace names the caller rather than a bare line in dmesg.
 */
static void aw_move_notify(struct dma_buf_attachment *attach)
{
	struct aw_buffer *b = attach->importer_priv;

	if (b)
		WRITE_ONCE(b->moved, true);
	WARN(1, "arcwell: move_notify fired on a PINNED attachment -- buffer %u poisoned, its pages are stale; in-flight bios cannot be recalled\n",
	     b ? b->handle : 0);
}

static const struct dma_buf_attach_ops aw_attach_ops = {
	.allow_peer2peer = true,		/* THE D3 FIX */
	.move_notify = aw_move_notify,
};

static struct pci_dev *find_by_bdf(const char *s)
{
	unsigned int dom, bus, slot, fn;

	if (sscanf(s, "%x:%x:%x.%x", &dom, &bus, &slot, &fn) != 4)
		return NULL;
	return pci_get_domain_bus_and_slot(dom, bus, PCI_DEVFN(slot, fn));
}

/* Carve granularity. Measured 2026-09-15: with 2 MiB granules, 12 BO
 * registrations produced 12 distinct xe placements and SIX permanent carves --
 * xe never reuses an address, and a carve is devres-bound to the pci_dev until
 * the device is rebound. At that rate a real streaming workload exhausts BAR2.
 *
 * The fix is bigger granules. memremap_pages() conflicts within a 128 MiB
 * SECTION anyway ("Conflicting mapping in same section", mm/memremap.c:158), so
 * section-sized carves are both the natural unit and the one that cannot
 * self-conflict. The 12 placements above spanned ~40 MiB: a single 128 MiB carve
 * would have covered all of them.
 *
 * Cost is struct pages, 64 B per 4 KiB = 1/64 of the carved range: 2 MiB of host
 * RAM per 128 MiB carve. That is housekeeping metadata, not a data buffer, so it
 * is within the project's no-host-RAM rule -- but it is not free, which is why we
 * carve on demand rather than mapping the whole BAR up front (that would cost
 * 256 MiB for the A770's 16 GiB and 512 MiB for the B60's 32 GiB).
 *
 * AW_CARVE_FALLBACK exists because a BAR that already holds small carves from an
 * earlier session cannot take an overlapping section-sized one. We then fall back
 * to the subsection granule rather than failing the registration.
 */
#define AW_CARVE_ALIGN    (128ULL << 20)	/* PA_SECTION_SIZE on x86 */
#define AW_CARVE_FALLBACK (2ULL << 20)		/* subsection granularity */

static bool carve_covers(phys_addr_t start, size_t len)
{
	struct aw_carve *c;

	list_for_each_entry(c, &g_carves, node)
		if (start >= c->start && start + len <= c->start + c->len)
			return true;
	return false;
}

static bool carve_overlaps(phys_addr_t start, size_t len)
{
	struct aw_carve *c;

	list_for_each_entry(c, &g_carves, node)
		if (start < c->start + c->len && c->start < start + len)
			return true;
	return false;
}

static int aw_try_carve(phys_addr_t phys, size_t len, u64 align)
{
	phys_addr_t a_start = ALIGN_DOWN(phys, align);
	size_t a_len = ALIGN(phys + len - a_start, align);
	phys_addr_t limit = g_bar_base + (vram_usable ? vram_usable : g_bar_len);
	struct aw_carve *c;
	int rc;

	/* Two different failures, and the caller must tell them apart:
	 *   -ERANGE  the requested range itself lies outside usable VRAM. Hopeless;
	 *            no granule helps.
	 *   -ENOSPC  the range is fine but THIS granule rounds up past the limit.
	 *            A smaller granule may well fit, so the caller keeps trying.
	 * Conflating them broke the clamp the moment it was added: a BO at
	 * 0x65f8b00000 fits in a 2 MiB carve but not a 128 MiB one, and the loop
	 * gave up at the first granule. */
	if (phys < g_bar_base || phys + len > limit)
		return -ERANGE;
	if (carve_overlaps(a_start, a_len))
		return -EEXIST;
	if (a_start < g_bar_base || a_start + a_len > limit)
		return -ENOSPC;

	rc = pci_p2pdma_add_resource(g_arc, bar_idx, a_len,
				     (u64)(a_start - g_bar_base));
	if (rc)
		return rc;

	/* The carve outlives this module: it is devres-owned by the pci_dev and is
	 * only released when the GPU is rebound. Runtime suspend on a carved GPU
	 * wedges it (see the header of this file). A pm_runtime_get_sync() held for
	 * the module's lifetime is therefore NOT enough -- the moment arcwell is
	 * unloaded the reference goes away while the carve remains, and the next
	 * idle period kills the card. That is not theoretical: it wedged the A770
	 * twice on 2026-09-15, the second time after the hazard had been written
	 * down.
	 *
	 * pm_runtime_forbid() is device state, not module state: it pins
	 * power/control to "on" and survives rmmod. Once a BAR is carved, runtime PM
	 * on that GPU stays off until the carve is gone, which means until the
	 * device is rebound or the machine reboots.
	 */
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;	/* the carve itself is devres-owned; nothing to undo */
	c->start = a_start;
	c->len = a_len;
	list_add(&c->node, &g_carves);
	g_carve_bytes += a_len;
	g_carve_count++;
	pr_info("arcwell: carved BAR%d [%pa +%zx) as P2P memory (%u carves, %llu MiB total)\n",
		bar_idx, &a_start, a_len, g_carve_count, g_carve_bytes >> 20);
	return 0;
}

/* Do valid P2PDMA struct pages ALREADY exist for this range?
 *
 * This replaces "is it in my carve list?". A carve is devres-owned by the
 * pci_dev and outlives the module, so after an rmmod/insmod the list is empty
 * while the device is still carved. Bookkeeping said "not carved", the carve
 * then failed -ENOMEM ("Conflicting mapping in same section"), and a perfectly
 * usable range was rejected. Probe the actual state instead of tracking it.
 */
static bool aw_pages_present(phys_addr_t phys, size_t len)
{
	unsigned long pfn = phys >> PAGE_SHIFT;
	unsigned long end = (phys + len - 1) >> PAGE_SHIFT;

	for (; pfn <= end; pfn++) {
		if (!pfn_valid(pfn))
			return false;
		if (!is_pci_p2pdma_page(pfn_to_page(pfn)))
			return false;
	}
	return true;
}

/* Runtime suspend on a carved GPU wedges it, and a carve outlives this module.
 * The protection must therefore key off "this BAR IS carved", not "this module
 * instance carved it" -- an instance that finds existing carves and reuses them
 * would otherwise leave the GPU unprotected, which is the most dangerous case of
 * all. Learned by failing a cell that asserted exactly this. */
static bool g_pm_forbidden;

static void aw_forbid_pm_once(void)
{
	if (g_pm_forbidden)
		return;
	pm_runtime_forbid(&g_arc->dev);
	g_pm_forbidden = true;
	pr_warn("arcwell: runtime PM FORBIDDEN on %s for as long as its BAR is "
		"carved. This is device state and outlives the module deliberately. "
		"Re-enable with 'echo auto > /sys/bus/pci/devices/%s/power/control' "
		"only once the carves are gone (rebind or reboot).\n",
		pci_name(g_arc), pci_name(g_arc));
}

static int aw_ensure_carved(phys_addr_t phys, size_t len)
{
	int rc;

	/* Cheapest and most reliable: the pages may already be there, whether we
	 * made them or a previous instance of this module did. */
	if (aw_pages_present(phys, len)) {
		aw_forbid_pm_once();	/* carved BAR, even if not by us */
		if (!carve_covers(phys, len))
			pr_info("arcwell: P2P pages already present for %pa +%zx "
				"(carved before this module instance); no carve needed\n",
				&phys, len);
		return 0;
	}

	if (carve_covers(phys, len)) {
		aw_forbid_pm_once();
		return 0;
	}

	/* Take the LARGEST aligned granule that fits without overlapping an
	 * existing carve, walking down by halves.
	 *
	 * All-or-nothing was wrong: whichever granule got used first in a 128 MiB
	 * section locked that section to that granule forever, because every later
	 * section-sized attempt overlapped the module's own earlier small carve.
	 * Measured before this change: 20 registrations -> 10 carves, every one of
	 * them the 2 MiB fallback, "section carve failed (-17)" each time.
	 */
	for (u64 g = AW_CARVE_ALIGN; g >= AW_CARVE_FALLBACK; g >>= 1) {
		rc = aw_try_carve(phys, len, g);
		if (!rc) {
			aw_forbid_pm_once();
			return 0;
		}
		if (rc == -ERANGE)	/* the range itself is outside usable VRAM */
			break;
		/* -ENOSPC (granule overruns the limit) and -EEXIST (overlaps an
		 * existing carve) both mean: try a smaller granule. */
	}

	/* A BAR already holding smaller carves cannot take an overlapping
	 * section-sized one. Two ways that shows up, and BOTH must fall back:
	 *   -EEXIST  our own carve list knows about the overlap;
	 *   -ENOMEM  the carve predates this module instance (e.g. an earlier
	 *            insmod), so our list is empty and memremap_pages() is the one
	 *            that objects -- "Conflicting mapping in same section", with a
	 *            kernel WARN. Unavoidable on a dirty BAR; harmless.
	 * Anything else is also worth one fallback attempt before giving up. */
	pr_err("arcwell: could not carve for phys %pa len %zx at any granule "
	       "(%llu MiB down to %llu MiB): %d\n",
	       &phys, len, AW_CARVE_ALIGN >> 20, AW_CARVE_FALLBACK >> 20, rc);
	return rc;
}

static void aw_free_buffer(struct aw_buffer *b)
{
	if (b->sgt) {
		dma_resv_lock(b->dbuf->resv, NULL);
		dma_buf_unmap_attachment(b->attach, b->sgt, DMA_FROM_DEVICE);
		dma_buf_unpin(b->attach);
		dma_resv_unlock(b->dbuf->resv);
	}
	if (b->attach)
		dma_buf_detach(b->dbuf, b->attach);
	if (b->dbuf)
		dma_buf_put(b->dbuf);
	kfree(b->pages);
	kfree(b);
}

static void aw_buffer_release(struct kref *kref)
{
	aw_free_buffer(container_of(kref, struct aw_buffer, ref));
}

/* Look a handle up in THIS client and take a reference, so a concurrent
 * AW_IOC_UNMAP_BUFFER cannot free the buffer out from under an in-flight
 * transfer. The caller drops it with kref_put(). */
static struct aw_buffer *aw_find_get(struct aw_client *cl, u32 handle)
{
	struct aw_buffer *b;

	xa_lock(&cl->buffers);
	b = xa_load(&cl->buffers, handle);
	if (b && unlikely(READ_ONCE(b->moved)))
		b = NULL;	/* poisoned by move_notify; see aw_move_notify */
	if (b)
		kref_get(&b->ref);
	xa_unlock(&cl->buffers);
	return b;
}

static long aw_map_buffer(struct aw_client *cl, void __user *uarg)
{
	struct aw_ioc_map_buffer arg;
	struct aw_buffer *b;
	struct iommu_domain *dom;
	struct scatterlist *sg;
	unsigned int i, pg = 0;
	long rc;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.in_source != AW_BUF_DMABUF) {
		pr_err("arcwell: only AW_BUF_DMABUF is implemented\n");
		return -EOPNOTSUPP;
	}

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	b->dbuf = dma_buf_get((int)arg.in_handle);
	if (IS_ERR(b->dbuf)) { rc = PTR_ERR(b->dbuf); b->dbuf = NULL; goto err; }

	/* --- step 1: dynamic attach declaring peer2peer --- */
	/* importer_priv is us: move_notify needs to reach the buffer to poison
	 * it, and the attachment is the only handle it is given. */
	b->attach = dma_buf_dynamic_attach(b->dbuf, &g_nvme->dev,
					   &aw_attach_ops, b);
	if (IS_ERR(b->attach)) { rc = PTR_ERR(b->attach); b->attach = NULL; goto err; }

	/* --- step 2: THE CONTRACT. xe clears this silently. --- */
	if (!b->attach->peer2peer || unlikely(debug_force_bounce)) {
		mutex_lock(&g_lock);
		g_stats.via_host_bounce++;	/* detected and refused, not performed */
		mutex_unlock(&g_lock);
		pr_err("arcwell: exporter refused peer2peer; a mapping here would be a "
		       "HOST BOUNCE. Failing per AW_MAP_F_REQUIRE_P2P.\n");
		rc = -EOPNOTSUPP;
		goto err;
	}

	/* --- step 3: pin and map --- */
	dma_resv_lock(b->dbuf->resv, NULL);
	rc = dma_buf_pin(b->attach);
	if (!rc) {
		b->sgt = dma_buf_map_attachment(b->attach, DMA_FROM_DEVICE);
		if (IS_ERR(b->sgt)) { rc = PTR_ERR(b->sgt); b->sgt = NULL;
				      dma_buf_unpin(b->attach); }
	}
	dma_resv_unlock(b->dbuf->resv);
	if (rc) goto err;

	/* --- step 4+5: IOVA -> BAR2 phys -> carve -> struct pages --- */
	dom = iommu_get_domain_for_dev(&g_nvme->dev);
	for_each_sgtable_sg(b->sgt, sg, i)
		b->npages += sg_dma_len(sg) >> PAGE_SHIFT;

	b->pages = kcalloc(b->npages, sizeof(*b->pages), GFP_KERNEL);
	if (!b->pages) { rc = -ENOMEM; goto err; }

	for_each_sgtable_sg(b->sgt, sg, i) {
		dma_addr_t iova = sg_dma_address(sg);
		unsigned int len = sg_dma_len(sg), o;
		phys_addr_t phys = dom ? iommu_iova_to_phys(dom, iova)
				       : (phys_addr_t)iova;

		if (!phys) {
			pr_err("arcwell: iommu_iova_to_phys(%pad) failed\n", &iova);
			rc = -EFAULT; goto err;
		}
		pr_info("arcwell: sg[%u] iova=%pad -> phys=%pa len=%u\n",
			i, &iova, &phys, len);

		rc = aw_ensure_carved(phys, len);
		if (rc) goto err;

		for (o = 0; o < len; o += PAGE_SIZE) {
			struct page *p = pfn_to_page((phys + o) >> PAGE_SHIFT);

			if (!is_pci_p2pdma_page(p)) {
				mutex_lock(&g_lock);
				g_stats.via_host_bounce++;	/* system memory, not VRAM */
				mutex_unlock(&g_lock);
				pr_err("arcwell: page at %pa is not a P2PDMA page: this sg is "
				       "system memory, which is the host bounce. Refusing.\n",
				       &phys);
				rc = -EFAULT; goto err;
			}
			b->pages[pg++] = p;
		}
	}
	b->npages = pg;
	b->length = (size_t)pg << PAGE_SHIFT;

	kref_init(&b->ref);
	mutex_lock(&g_lock);
	b->handle = g_next_handle++;
	mutex_unlock(&g_lock);

	rc = xa_err(xa_store(&cl->buffers, b->handle, b, GFP_KERNEL));
	if (rc)
		goto err;
	mutex_lock(&cl->lock);
	if (++cl->live > cl->peak)
		cl->peak = cl->live;
	mutex_unlock(&cl->lock);

	arg.out_handle = b->handle;
	arg.out_flags = AW_MAP_F_REQUIRE_P2P;
	if (copy_to_user(uarg, &arg, sizeof(arg)))
		return -EFAULT;

	pr_info("arcwell: MAP_BUFFER handle=%u npages=%u bytes=%zu peer2peer=1 (live=%u peak=%u)\n",
		b->handle, b->npages, b->length, cl->live, cl->peak);
	return 0;
err:
	aw_free_buffer(b);
	return rc;
}

/* ---- shared submission machinery ---------------------------------------
 * M4_API.md: "A batch of experts is the inference pattern, not a single bulk
 * read." Both the single read and the batch submit through here so completion
 * accounting is per-BIO, which is what BIO_MAX_VECS splitting makes necessary.
 */
static void aw_batch_endio(struct bio *bio)
{
	struct aw_batch *ba = bio->bi_private;

	if (bio->bi_status && !ba->status)
		ba->status = bio->bi_status;
	bio_put(bio);
	/* complete_all(), not complete(): it leaves the completion permanently
	 * signalled, so a second observer (a poll that races the collector, or
	 * release draining) sees "done" instead of parking forever on a
	 * completion nobody will signal again. */
	if (atomic_dec_and_test(&ba->pending))
		complete_all(&ba->done);
}

/* Submit one request as however many bios BIO_MAX_VECS requires.
 *
 * A bio holds at most BIO_MAX_VECS (256) vectors; bio_alloc() with more does not
 * fail, it hits `default: BUG()` in biovec_slab() -- block/bio.c:61. A 2,457,600
 * byte expert slice is 600 pages, so a realistic transfer BUGs the kernel from a
 * userspace-supplied length. Every earlier cell in this campaign used 1 MiB, i.e.
 * exactly 256 pages, and sat precisely on the limit without crossing it; the
 * defect was latent from the first version of this module and surfaced the first
 * time a real expert size was used. Measured 2026-09-15: four Oopses, kernel
 * tainted [D]=DIE.
 *
 * Each bio is counted in @ba, so accounting is per-bio, not per-request.
 * Callers must hold a reference on @b.
 */
static int aw_submit_request(struct aw_buffer *b, const struct aw_ioc_read_blocks *r,
			     struct aw_batch *ba, unsigned int *nbio)
{
	unsigned int first, npg, done = 0;
	u64 bytes = r->in_block_count << 9;

	if (!bytes || (r->in_dest_offset & ~PAGE_MASK) || (bytes & ~PAGE_MASK))
		return -EINVAL;
	first = r->in_dest_offset >> PAGE_SHIFT;
	npg = bytes >> PAGE_SHIFT;
	if (first + npg > b->npages)
		return -ERANGE;

	while (done < npg) {
		unsigned int want = min_t(unsigned int, npg - done, BIO_MAX_VECS);
		struct bio *bio = bio_alloc(g_bdev, want, REQ_OP_READ, GFP_KERNEL);
		unsigned int added = 0;

		if (!bio)
			return -ENOMEM;	/* bios already in flight still complete */
		/* PAGE_SHIFT-9: one page is 8 x 512 B sectors */
		bio->bi_iter.bi_sector = r->in_start_block + ((u64)done << (PAGE_SHIFT - 9));

		/* A failed bio_add_page() means FULL, not broken: submit what is in
		 * hand and continue. Treating it as an error produced err=-5 after the
		 * BIO_MAX_VECS fix.
		 *
		 * Measured, against my own first guess: this queue reports
		 * max_sectors_kb=128 (32 pages), but bio_add_page does NOT enforce it --
		 * the observed split is 3 bios per 600-page expert, i.e. 256-page
		 * chunks, so BIO_MAX_VECS is what binds here and blk-mq applies
		 * max_sectors later via blk_queue_split() at submission. The exact
		 * trigger of the earlier -EIO was not isolated; this loop is correct
		 * regardless of which limit bites, which is the point of not hardcoding
		 * one. */
		while (added < want) {
			if (!bio_add_page(bio, b->pages[first + done + added], PAGE_SIZE, 0))
				break;
			added++;
		}
		if (!added) {	/* could not place even one page: a real failure */
			bio_put(bio);
			return -EIO;
		}
		bio->bi_private = ba;
		bio->bi_end_io = aw_batch_endio;
		atomic_inc(&ba->pending);
		submit_bio(bio);
		done += added;
		(*nbio)++;
	}
	return 0;
}

static long aw_read_blocks(struct aw_client *cl, void __user *uarg)
{
	struct aw_ioc_read_blocks arg;
	struct aw_buffer *b;
	struct aw_batch ba;
	unsigned int nbio = 0;
	ktime_t t0;
	long rc;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	b = aw_find_get(cl, (u32)arg.in_buffer_handle);
	if (!b)
		return -EINVAL;

	/* One request, but still split across bios at BIO_MAX_VECS, so the same
	 * completion accounting is used as for a batch. */
	atomic_set(&ba.pending, 1);
	init_completion(&ba.done);
	ba.status = 0;
	t0 = ktime_get();

	rc = aw_submit_request(b, &arg, &ba, &nbio);

	if (atomic_dec_and_test(&ba.pending))
		complete_all(&ba.done);
	wait_for_completion(&ba.done);

	if (!rc && ba.status)
		rc = blk_status_to_errno(ba.status);
	if (rc)
		goto out_put;

	mutex_lock(&g_lock);
	g_stats.reads++;
	g_stats.bytes += arg.in_block_count << 9;
	g_stats.us_total += ktime_us_delta(ktime_get(), t0);
	g_stats.segments += nbio;
	if (nbio > g_stats.max_inflight)
		g_stats.max_inflight = nbio;
	mutex_unlock(&g_lock);

	arg.out_bytes = arg.in_block_count << 9;
	arg.out_segments = nbio;	/* real bios, not page count */
	rc = copy_to_user(uarg, &arg, sizeof(arg)) ? -EFAULT : 0;
out_put:
	kref_put(&b->ref, aw_buffer_release);
	return rc;
}

static long aw_read_batch(struct aw_client *cl, void __user *uarg)
{
	struct aw_ioc_read_batch hdr;
	struct aw_ioc_read_blocks *reqs = NULL;
	struct aw_buffer **refs = NULL;
	struct aw_batch ba;
	unsigned int i, submitted = 0, nbio = 0;
	u64 bytes = 0;
	ktime_t t0;
	long rc = 0;

	if (copy_from_user(&hdr, uarg, sizeof(hdr)))
		return -EFAULT;
	if (!hdr.in_count || hdr.in_count > AW_BATCH_MAX)
		return -EINVAL;

	reqs = kvmalloc_array(hdr.in_count, sizeof(*reqs), GFP_KERNEL);
	refs = kcalloc(hdr.in_count, sizeof(*refs), GFP_KERNEL);
	if (!reqs || !refs) { rc = -ENOMEM; goto out; }
	if (copy_from_user(reqs, (void __user *)(uintptr_t)hdr.in_requests,
			   (size_t)hdr.in_count * sizeof(*reqs))) {
		rc = -EFAULT; goto out;
	}

	hdr.out_err = 0;
	hdr.out_err_index = 0;
	atomic_set(&ba.pending, 1);	/* the submitter's own reference */
	init_completion(&ba.done);
	ba.status = 0;
	t0 = ktime_get();

	for (i = 0; i < hdr.in_count; i++) {
		int brc;

		refs[i] = aw_find_get(cl, (u32)reqs[i].in_buffer_handle);
		if (!refs[i]) { brc = -EINVAL; goto req_failed; }

		/* submits 1..n bios, all counted in ba; no wait here, that is the point */
		brc = aw_submit_request(refs[i], &reqs[i], &ba, &nbio);
		if (brc) goto req_failed;

		submitted++;
		bytes += reqs[i].in_block_count << 9;
		continue;
req_failed:
		if (!hdr.out_err) { hdr.out_err = brc; hdr.out_err_index = i; }
		break;			/* stop submitting; still wait for what is in flight */
	}

	/* Drop the submitter's reference; the last completion wakes us. */
	if (atomic_dec_and_test(&ba.pending))
		complete_all(&ba.done);
	wait_for_completion(&ba.done);

	if (ba.status && !hdr.out_err)
		hdr.out_err = blk_status_to_errno(ba.status);

	mutex_lock(&g_lock);
	g_stats.batches++;
	g_stats.batch_reads += submitted;
	g_stats.reads += submitted;
	g_stats.segments += nbio;	/* real bios; the uAPI documents this as
					 * making gather visible, so it must move */
	g_stats.bytes += hdr.out_err ? 0 : bytes;
	g_stats.us_total += ktime_us_delta(ktime_get(), t0);
	if (nbio > g_stats.max_inflight)
		g_stats.max_inflight = nbio;
	mutex_unlock(&g_lock);

	hdr.out_bytes = hdr.out_err ? 0 : bytes;
	hdr.out_completed = hdr.out_err ? 0 : submitted;
	/* out_segments now reports real bios, not page count */
	if (copy_to_user(uarg, &hdr, sizeof(hdr)))
		rc = -EFAULT;
	else
		rc = hdr.out_err;

	pr_info("arcwell: READ_BATCH n=%u submitted=%u bytes=%llu err=%d\n",
		hdr.in_count, submitted, hdr.out_bytes, hdr.out_err);
out:
	if (refs)
		for (i = 0; i < hdr.in_count; i++)
			if (refs[i])
				kref_put(&refs[i]->ref, aw_buffer_release);
	kfree(refs);
	kvfree(reqs);
	return rc;
}

/* Collect a finished batch: drop buffer refs, account, free. */
static void aw_inflight_finish(struct aw_client *cl, struct aw_inflight *inf,
			       struct aw_ioc_batch_wait *out)
{
	unsigned int i;

	out->out_err = inf->ba.status ? blk_status_to_errno(inf->ba.status) : 0;
	out->out_bytes = out->out_err ? 0 : inf->bytes;
	out->out_completed = out->out_err ? 0 : inf->submitted;
	out->out_segments = inf->nbio;

	mutex_lock(&g_lock);
	g_stats.batches++;
	g_stats.batch_reads += inf->submitted;
	g_stats.reads += inf->submitted;
	g_stats.bytes += out->out_bytes;
	g_stats.segments += inf->nbio;
	if (inf->nbio > g_stats.max_inflight)
		g_stats.max_inflight = inf->nbio;
	mutex_unlock(&g_lock);

	for (i = 0; i < inf->nrefs; i++)
		if (inf->refs[i])
			kref_put(&inf->refs[i]->ref, aw_buffer_release);
	kfree(inf->refs);
	kfree(inf);
}

static long aw_submit_batch(struct aw_client *cl, void __user *uarg)
{
	struct aw_ioc_batch_submit hdr;
	struct aw_ioc_read_blocks *reqs = NULL;
	struct aw_inflight *inf = NULL;
	unsigned int i;
	long rc = 0;

	if (copy_from_user(&hdr, uarg, sizeof(hdr)))
		return -EFAULT;
	if (!hdr.in_count || hdr.in_count > AW_BATCH_MAX)
		return -EINVAL;

	reqs = kvmalloc_array(hdr.in_count, sizeof(*reqs), GFP_KERNEL);
	inf = kzalloc(sizeof(*inf), GFP_KERNEL);
	if (!reqs || !inf) { rc = -ENOMEM; goto err; }
	inf->refs = kcalloc(hdr.in_count, sizeof(*inf->refs), GFP_KERNEL);
	if (!inf->refs) { rc = -ENOMEM; goto err; }
	inf->nrefs = hdr.in_count;
	if (copy_from_user(reqs, (void __user *)(uintptr_t)hdr.in_requests,
			   (size_t)hdr.in_count * sizeof(*reqs))) {
		rc = -EFAULT; goto err;
	}

	atomic_set(&inf->ba.pending, 1);	/* the submitter's own reference */
	init_completion(&inf->ba.done);
	inf->ba.status = 0;
	hdr.out_err = 0;

	for (i = 0; i < hdr.in_count; i++) {
		int brc;

		inf->refs[i] = aw_find_get(cl, (u32)reqs[i].in_buffer_handle);
		if (!inf->refs[i]) { brc = -EINVAL; goto req_failed; }
		brc = aw_submit_request(inf->refs[i], &reqs[i], &inf->ba, &inf->nbio);
		if (brc) goto req_failed;
		inf->submitted++;
		inf->bytes += reqs[i].in_block_count << 9;
		continue;
req_failed:
		if (!hdr.out_err)
			hdr.out_err = brc;
		break;
	}

	mutex_lock(&cl->lock);
	inf->id = ++cl->next_batch_id;
	mutex_unlock(&cl->lock);

	rc = xa_err(xa_store(&cl->batches, inf->id, inf, GFP_KERNEL));
	if (rc) {
		/* cannot track it, so we must not return without waiting */
		if (atomic_dec_and_test(&inf->ba.pending))
			complete_all(&inf->ba.done);
		wait_for_completion(&inf->ba.done);
		goto err;
	}

	/* Drop the submitter's reference. From here the last completion wakes it.
	 * NOTE: we do NOT wait. That is the entire point of this call. */
	if (atomic_dec_and_test(&inf->ba.pending))
		complete_all(&inf->ba.done);

	hdr.out_batch_id = inf->id;
	hdr.out_submitted = inf->submitted;
	kvfree(reqs);
	return copy_to_user(uarg, &hdr, sizeof(hdr)) ? -EFAULT : 0;
err:
	if (inf) {
		for (i = 0; i < inf->nrefs; i++)
			if (inf->refs[i])
				kref_put(&inf->refs[i]->ref, aw_buffer_release);
		kfree(inf->refs);
		kfree(inf);
	}
	kvfree(reqs);
	return rc;
}

static long aw_batch_wait(struct aw_client *cl, void __user *uarg)
{
	struct aw_ioc_batch_wait arg;
	struct aw_inflight *inf;
	long left;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	/* Claim the batch before waiting on it. Look-up and claim must be one
	 * atomic step against cl->lock, or two collectors both get the pointer
	 * and the first to finish frees it under the second. */
	mutex_lock(&cl->lock);
	inf = xa_load(&cl->batches, arg.in_batch_id);
	if (!inf) {
		mutex_unlock(&cl->lock);
		return -EINVAL;
	}
	if (inf->collecting) {
		mutex_unlock(&cl->lock);
		return -EBUSY;		/* another thread is collecting this id */
	}
	inf->collecting = true;
	mutex_unlock(&cl->lock);

	if (arg.in_timeout_us == 0) {
		if (!try_wait_for_completion(&inf->ba.done))
			goto again;		/* still in flight; id stays valid */
	} else if (arg.in_timeout_us == U64_MAX) {
		wait_for_completion(&inf->ba.done);
	} else {
		left = wait_for_completion_timeout(&inf->ba.done,
					usecs_to_jiffies(arg.in_timeout_us) + 1);
		if (!left)
			goto again;
	}

	/* We hold the claim, so this erase cannot lose a race -- but check it
	 * rather than assume it, because an ignored xa_erase() return is exactly
	 * how the double free got in. */
	mutex_lock(&cl->lock);
	if (xa_erase(&cl->batches, arg.in_batch_id) != inf) {
		inf->collecting = false;
		mutex_unlock(&cl->lock);
		return -EINVAL;
	}
	mutex_unlock(&cl->lock);

	aw_inflight_finish(cl, inf, &arg);
	return copy_to_user(uarg, &arg, sizeof(arg)) ? -EFAULT : 0;

again:
	/* Not finished: drop the claim so the caller (or another thread) can
	 * poll again. The batch stays in the xarray and stays collectable. */
	mutex_lock(&cl->lock);
	inf->collecting = false;
	mutex_unlock(&cl->lock);
	return -EAGAIN;
}

static long aw_unmap_buffer(struct aw_client *cl, void __user *uarg)
{
	struct aw_buffer *found;
	u32 handle;

	if (copy_from_user(&handle, uarg, sizeof(handle)))
		return -EFAULT;

	found = xa_erase(&cl->buffers, handle);
	if (!found)
		return -EINVAL;
	mutex_lock(&cl->lock);
	cl->live--;
	mutex_unlock(&cl->lock);

	pr_info("arcwell: UNMAP_BUFFER handle=%u (live=%u peak=%u)\n",
		handle, cl->live, cl->peak);
	kref_put(&found->ref, aw_buffer_release);
	return 0;
}

static int aw_open(struct inode *ino, struct file *f)
{
	struct aw_client *cl = kzalloc(sizeof(*cl), GFP_KERNEL);

	if (!cl)
		return -ENOMEM;
	xa_init_flags(&cl->buffers, XA_FLAGS_ALLOC);
	xa_init(&cl->batches);
	mutex_init(&cl->lock);
	f->private_data = cl;
	return 0;
}

/* Everything this client registered goes away with its fd. A crashed client
 * cannot strand pinned VRAM. */
static int aw_release_file(struct inode *ino, struct file *f)
{
	struct aw_client *cl = f->private_data;
	struct aw_buffer *b;
	unsigned long idx;

	if (!cl)
		return 0;
	if (cl->live)
		pr_info("arcwell: releasing %u buffer(s) still registered at close\n",
			cl->live);
	/* Drain in-flight batches FIRST. Their bios point at &inf->ba, so freeing
	 * an uncollected batch while a transfer is live would corrupt memory from
	 * the completion path. */
	{
		struct aw_inflight *inf;
		unsigned long bid;
		struct aw_ioc_batch_wait discard;

		xa_for_each(&cl->batches, bid, inf) {
			wait_for_completion(&inf->ba.done);
			xa_erase(&cl->batches, bid);
			aw_inflight_finish(cl, inf, &discard);
		}
		xa_destroy(&cl->batches);
	}
	xa_for_each(&cl->buffers, idx, b)
		kref_put(&b->ref, aw_buffer_release);
	xa_destroy(&cl->buffers);
	mutex_destroy(&cl->lock);
	kfree(cl);
	return 0;
}

static long aw_ioctl(struct file *f, unsigned int cmd, unsigned long a)
{
	struct aw_client *cl = f->private_data;

	if (!cl)
		return -EBADF;

	switch (cmd) {
	case AW_IOC_MAP_BUFFER:	   return aw_map_buffer(cl, (void __user *)a);
	case AW_IOC_READ_BLOCKS:   return aw_read_blocks(cl, (void __user *)a);
	case AW_IOC_READ_BATCH:    return aw_read_batch(cl, (void __user *)a);
	case AW_IOC_SUBMIT_BATCH:  return aw_submit_batch(cl, (void __user *)a);
	case AW_IOC_BATCH_WAIT:    return aw_batch_wait(cl, (void __user *)a);
	case AW_IOC_UNMAP_BUFFER:  return aw_unmap_buffer(cl, (void __user *)a);
	case AW_IOC_STATS: {
		struct aw_ioc_stats st;

		mutex_lock(&g_lock);
		st = g_stats;
		mutex_unlock(&g_lock);
		mutex_lock(&cl->lock);
		st.buffers_live = cl->live;
		st.buffers_peak = cl->peak;
		{
			struct aw_inflight *q; unsigned long bi; u32 n = 0;

			xa_for_each(&cl->batches, bi, q) n++;
			st.batches_inflight = n;
		}
		mutex_unlock(&cl->lock);
		return copy_to_user((void __user *)a, &st, sizeof(st)) ? -EFAULT : 0;
	}
	}
	return -ENOTTY;
}

static const struct file_operations aw_fops = {
	.owner = THIS_MODULE,
	.open = aw_open,
	.release = aw_release_file,
	.unlocked_ioctl = aw_ioctl,
};

static struct miscdevice aw_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = AW_DEVICE_NAME,
	.fops = &aw_fops,
};

static int __init aw_init(void)
{
	int rc;

	if (!arc_bdf[0] || !nvme_bdf[0]) {
		pr_err("arcwell: arc_bdf= and nvme_bdf= are MANDATORY\n");
		return -EINVAL;
	}
	g_arc = find_by_bdf(arc_bdf);
	g_nvme = find_by_bdf(nvme_bdf);
	if (!g_arc || !g_nvme) { rc = -ENODEV; goto err; }

	g_bar_base = pci_resource_start(g_arc, bar_idx);
	g_bar_len = pci_resource_len(g_arc, bar_idx);

	g_bf = bdev_file_open_by_path(bdev_path, BLK_OPEN_READ, NULL, NULL);
	if (IS_ERR(g_bf)) { rc = PTR_ERR(g_bf); g_bf = NULL; goto err; }
	g_bdev = file_bdev(g_bf);

	if (!blk_queue_pci_p2pdma(bdev_get_queue(g_bdev))) {
		pr_err("arcwell: %s queue does not support PCI P2PDMA\n", bdev_path);
		rc = -EOPNOTSUPP;
		goto err;
	}
	/* Hold the GPU runtime-PM-awake for as long as arcwell is loaded.
	 *
	 * MEASURED THE HARD WAY, 2026-09-15: a p2pdma carve on BAR2 is incompatible
	 * with GPU runtime suspend. The carve creates ZONE_DEVICE pages backed by
	 * the BAR; when xe then tries to drop the device to D3 the suspend times
	 * out --
	 *     xe 0000:06:00.0: [drm] *ERROR* Tile0: GT0: runtime suspend failed (-ETIMEDOUT)
	 *     xe 0000:06:00.0: Runtime PM usage count underflow!
	 * -- the device lands in power/runtime_status=error, and from that moment
	 * EVERY DRM_IOCTL_XE_GEM_CREATE on it returns -EINVAL, for every process on
	 * the machine. It does not recover from `echo on > power/control`; it needs
	 * a driver rebind or a reboot. This wedged the A770 during this campaign.
	 *
	 * Taking a runtime PM reference stops the suspend attempt from ever being
	 * made. NOTE THE RESIDUAL HAZARD: carves are devres-owned by the pci_dev and
	 * outlive this module, so after rmmod the BAR is still carved while nothing
	 * holds the GPU awake. Until carves can be released, do not leave a carved
	 * GPU idle with arcwell unloaded.
	 */
	pm_runtime_get_sync(&g_arc->dev);
	pr_info("arcwell: holding runtime PM on %s (carved BAR + D3 = wedged GPU)\n",
		pci_name(g_arc));

	if (carve_all) {
		u64 len = vram_usable ? vram_usable : g_bar_len;
		ktime_t t0 = ktime_get();

		pr_info("arcwell: carve_all: registering %llu MiB of VRAM up front "
			"(~%llu MiB of host RAM in struct pages)\n",
			len >> 20, (len >> 12) * 64 >> 20);
		rc = pci_p2pdma_add_resource(g_arc, bar_idx, (size_t)len, 0);
		if (rc) {
			pr_err("arcwell: carve_all failed: %d\n", rc);
			pm_runtime_put(&g_arc->dev);
			goto err;
		}
		aw_forbid_pm_once();
		{
			struct aw_carve *c = kzalloc(sizeof(*c), GFP_KERNEL);

			if (c) {
				c->start = g_bar_base;
				c->len = len;
				list_add(&c->node, &g_carves);
				g_carve_bytes = len;
				g_carve_count = 1;
			}
		}
		pr_info("arcwell: carve_all done in %lld ms; no further carves will occur\n",
			ktime_ms_delta(ktime_get(), t0));
	}

	rc = misc_register(&aw_misc);
	if (rc) { pm_runtime_put(&g_arc->dev); goto err; }

	if (!vram_usable)
		pr_warn("arcwell: vram_usable=0, falling back to the BAR length. The BAR is "
			"larger than usable VRAM on both Arc cards here; a carve near the "
			"top will cover stolen or absent memory. Pass the value from xe's "
			"\"usable size exclude stolen\" boot line.\n");
	pr_info("arcwell: ready. arc=%s BAR%d=%pa+%pa usable=%#llx nvme=%s bdev=%s\n",
		pci_name(g_arc), bar_idx, &g_bar_base, &g_bar_len,
		vram_usable ? vram_usable : (unsigned long long)g_bar_len,
		pci_name(g_nvme), bdev_path);
	return 0;
err:
	if (g_bf) fput(g_bf);
	if (g_nvme) pci_dev_put(g_nvme);
	if (g_arc) pci_dev_put(g_arc);
	return rc;
}

static void __exit aw_exit(void)
{
	struct aw_carve *c, *ct;

	/* Buffers are owned by their open file and are gone by now: misc_deregister
	 * cannot return while a descriptor is still open. */
	misc_deregister(&aw_misc);
	list_for_each_entry_safe(c, ct, &g_carves, node) {
		list_del(&c->node);
		kfree(c);
	}
	/* The carves themselves are devres-owned by the pci_dev and are NOT
	 * released here; they persist until the GPU is rebound. Say so, so nobody
	 * reads a clean unload as a clean BAR. */
	if (g_carve_count)
		pr_info("arcwell: %u carves (%llu MiB) remain registered on the GPU "
			"until it is rebound\n", g_carve_count, g_carve_bytes >> 20);
	if (g_bf) fput(g_bf);
	if (g_arc) {
		pm_runtime_put(&g_arc->dev);
		if (g_carve_count)
			pr_warn("arcwell: %u carve(s) remain on %s. Runtime PM stays "
				"FORBIDDEN on it (pm_runtime_forbid, device state, not "
				"module state) precisely so unloading this module cannot "
				"leave a carved GPU free to suspend and wedge itself.\n",
				g_carve_count, pci_name(g_arc));
	}
	if (g_nvme) pci_dev_put(g_nvme);
	if (g_arc) pci_dev_put(g_arc);
	pr_info("arcwell: unloaded\n");
}

module_init(aw_init);
module_exit(aw_exit);
