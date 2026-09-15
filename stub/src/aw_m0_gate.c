// SPDX-License-Identifier: GPL-2.0
/* aw_m0_gate — BACKLOG item 4. THE M0 ACCEPTANCE CELL.
 *
 * Claim under test: the NVMe controller DMAs disk bytes DIRECTLY into Intel Arc
 * VRAM, with no host bounce buffer anywhere in the data path.
 *
 * Method:
 *   1. pci_p2pdma_add_resource() carves Arc BAR2 into ZONE_DEVICE P2P memory.
 *      (B1 was an ABI bug, not a kernel limit -- see BACKLOG.md D4.)
 *   2. pci_p2pmem_alloc_sgl() takes a chunk of that carve as real struct pages.
 *      KERNEL_FACTS.md rule honoured: alloc in modest chunks, never multi-GiB.
 *   3. Poison the target VRAM through BAR2 with 0xA5, so "unchanged" is visible.
 *   4. Build a bio from the P2P pages and submit_bio_wait() a READ. nvme.ko uses
 *      blk_rq_dma_map_iter_start/next on this kernel and handles P2P natively.
 *   5. CRC the VRAM by reading BAR2 back -- a path the GPU never wrote.
 *   6. CONTROL: read the same LBA range into ordinary host pages, CRC that.
 *   7. PASS iff the two CRCs match and the poison is gone.
 *
 * RED-FIRST (HOUSE_RULES.md §5). This cell is proven able to fail: mutate=1
 * reads the verification back from bar_off + xfer instead of the DMA target --
 * the destination is mutated, nothing wrote there, the poison survives, the CRCs
 * differ, and the cell MUST report FAIL. A cell that cannot fail is decoration.
 * Run mutate=1 first, paste the red, then mutate=0 for the green.
 *
 * THE BLOCK DEVICE IS OPENED READ-ONLY (BLK_OPEN_READ) AND IS NEVER WRITTEN.
 * The test reads existing bytes and compares; it never puts a pattern on disk.
 *
 * Load: insmod aw_m0_gate.ko arc_bdf=0000:06:00.0 nvme_bdf=0000:01:00.0 \
 *       bdev_path=/dev/nvme0n1 [xfer=0x100000] [disk_off=0x40000000] [mutate=0]
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/pci-p2pdma.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/crc32.h>
#include <linux/scatterlist.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("arcwell M0 acceptance cell: NVMe -> Arc VRAM by controller DMA");

static char *arc_bdf = "";
module_param(arc_bdf, charp, 0444);
static char *nvme_bdf = "";
module_param(nvme_bdf, charp, 0444);
static char *bdev_path = "/dev/nvme0n1";
module_param(bdev_path, charp, 0444);
static int bar_idx = 2;
module_param(bar_idx, int, 0444);
static unsigned long long bar_off = 0x200000000ULL;	/* 8 GiB into BAR2 */
module_param(bar_off, ullong, 0444);
static unsigned long long carve = 0x4000000ULL;		/* 64 MiB carve */
module_param(carve, ullong, 0444);
static unsigned int xfer = 0x100000;			/* 1 MiB transfer */
module_param(xfer, uint, 0444);
static unsigned long long disk_off = 0x40000000ULL;	/* 1 GiB in; read-only */
module_param(disk_off, ullong, 0444);
static bool mutate;
module_param(mutate, bool, 0444);
MODULE_PARM_DESC(mutate, "RED-FIRST: verify from the WRONG address; cell must FAIL");

#define POISON 0xA5

static struct pci_dev *find_by_bdf(const char *s)
{
	unsigned int dom, bus, slot, fn;

	if (sscanf(s, "%x:%x:%x.%x", &dom, &bus, &slot, &fn) != 4)
		return NULL;
	return pci_get_domain_bus_and_slot(dom, bus, PCI_DEVFN(slot, fn));
}

/* Walk the P2P sgl through BAR2 MMIO. off_bias != 0 is the red-first mutation:
 * it verifies a location the DMA never targeted. */
static u32 vram_crc(struct scatterlist *sgl, unsigned int nents,
		    u64 off_bias, bool fill_poison)
{
	struct scatterlist *sg;
	void *tmp = kmalloc(PAGE_SIZE, GFP_KERNEL);
	u32 crc = 0;
	unsigned int i;

	if (!tmp)
		return 0;

	for_each_sg(sgl, sg, nents, i) {
		phys_addr_t phys = page_to_phys(sg_page(sg)) + sg->offset + off_bias;
		unsigned int left = sg->length, done = 0;

		while (left) {
			unsigned int n = min_t(unsigned int, left, PAGE_SIZE);
			void __iomem *m = ioremap(phys + done, n);

			if (!m) {
				pr_err("aw_m0_gate: ioremap(%pa, %u) failed\n", &phys, n);
				kfree(tmp);
				return 0;
			}
			if (fill_poison) {
				memset_io(m, POISON, n);
			} else {
				memcpy_fromio(tmp, m, n);
				crc = crc32(crc, tmp, n);
			}
			iounmap(m);
			done += n;
			left -= n;
		}
	}
	kfree(tmp);
	return crc;
}

static int __init awm0_init(void)
{
	struct pci_dev *arc = NULL, *nvme = NULL;
	struct scatterlist *sgl = NULL, *sg;
	struct file *bf = NULL;
	struct block_device *bdev;
	struct bio *bio;
	struct page **hpages = NULL;
	unsigned int nents = 0, i, npg = xfer >> PAGE_SHIFT;
	u32 crc_poison, crc_vram, crc_host = 0;
	void *hbuf;
	int rc;

	if (!arc_bdf[0] || !nvme_bdf[0]) {
		pr_err("aw_m0_gate: arc_bdf= and nvme_bdf= are MANDATORY\n");
		return -EINVAL;
	}
	arc = find_by_bdf(arc_bdf);
	nvme = find_by_bdf(nvme_bdf);
	if (!arc || !nvme) { rc = -ENODEV; goto out; }

	pr_info("aw_m0_gate: ==== M0 ACCEPTANCE CELL %s ====\n",
		mutate ? "[MUTATED - MUST FAIL]" : "[normal - must PASS]");
	pr_info("aw_m0_gate: arc=%s nvme=%s bdev=%s xfer=%#x disk_off=%#llx\n",
		pci_name(arc), pci_name(nvme), bdev_path, xfer, disk_off);

	/* --- 1+2. ALLOC FIRST, carve only if needed.
	 * A carve persists on the pci_dev (devres) until the device is rebound, so
	 * re-adding the same range trips "Conflicting mapping in same section"
	 * (mm/memremap.c:158) and returns -ENOMEM. Sections are 128 MiB on x86, so
	 * even a different offset inside the same section conflicts. Probing with
	 * the allocator is idempotent and avoids the WARN entirely. --- */
	sgl = pci_p2pmem_alloc_sgl(arc, &nents, xfer);
	if (sgl) {
		pr_info("aw_m0_gate: p2pmem already carved; alloc_sgl succeeded directly\n");
	} else {
		rc = pci_p2pdma_add_resource(arc, bar_idx, (size_t)carve, (u64)bar_off);
		pr_info("aw_m0_gate: pci_p2pdma_add_resource(size=%#llx off=%#llx) = %d\n",
			carve, bar_off, rc);
		if (rc) goto out;
		sgl = pci_p2pmem_alloc_sgl(arc, &nents, xfer);
	}
	if (!sgl) {
		pr_err("aw_m0_gate: pci_p2pmem_alloc_sgl(%#x) FAILED\n", xfer);
		rc = -ENOMEM;
		goto out;
	}
	pr_info("aw_m0_gate: p2pmem sgl ok nents=%u\n", nents);
	for_each_sg(sgl, sg, nents, i) {
		phys_addr_t p = page_to_phys(sg_page(sg));

		pr_info("aw_m0_gate:   sg[%u] phys=%pa len=%u is_p2p=%d\n",
			i, &p, sg->length, is_pci_p2pdma_page(sg_page(sg)));
		if (i >= 3) { pr_info("aw_m0_gate:   ...\n"); break; }
	}

	/* --- 3. poison the VRAM so "nothing happened" is visible --- */
	vram_crc(sgl, nents, 0, true);
	crc_poison = vram_crc(sgl, nents, 0, false);
	pr_info("aw_m0_gate: VRAM poisoned with %#x, crc=%#x\n", POISON, crc_poison);

	/* --- 4. open the block device READ-ONLY and check P2P support --- */
	bf = bdev_file_open_by_path(bdev_path, BLK_OPEN_READ, NULL, NULL);
	if (IS_ERR(bf)) { rc = PTR_ERR(bf); bf = NULL;
		pr_err("aw_m0_gate: open %s = %d\n", bdev_path, rc); goto out; }
	bdev = file_bdev(bf);
	pr_info("aw_m0_gate: queue blk_queue_pci_p2pdma=%d\n",
		blk_queue_pci_p2pdma(bdev_get_queue(bdev)) ? 1 : 0);

	/* --- 5. the DMA: NVMe controller -> Arc VRAM --- */
	bio = bio_alloc(bdev, nents, REQ_OP_READ | REQ_SYNC, GFP_KERNEL);
	if (!bio) { rc = -ENOMEM; goto out; }
	bio->bi_iter.bi_sector = disk_off >> 9;
	for_each_sg(sgl, sg, nents, i) {
		if (!bio_add_page(bio, sg_page(sg), sg->length, sg->offset)) {
			pr_err("aw_m0_gate: bio_add_page rejected P2P page at sg[%u]\n", i);
			bio_put(bio);
			rc = -EIO;
			goto out;
		}
	}
	rc = submit_bio_wait(bio);
	pr_info("aw_m0_gate: submit_bio_wait() = %d\n", rc);
	bio_put(bio);
	if (rc) goto out;

	/* --- 6. verify by reading BAR2 back. The GPU never wrote here. --- */
	crc_vram = vram_crc(sgl, nents, mutate ? xfer : 0, false);
	pr_info("aw_m0_gate: VRAM crc after DMA = %#x %s\n", crc_vram,
		mutate ? "(READ FROM MUTATED ADDRESS)" : "");

	/* --- 7. CONTROL: same LBAs into ordinary host pages --- */
	hpages = kcalloc(npg, sizeof(*hpages), GFP_KERNEL);
	hbuf = vzalloc(xfer);
	if (!hpages || !hbuf) { rc = -ENOMEM; goto out; }
	for (i = 0; i < npg; i++)
		hpages[i] = vmalloc_to_page(hbuf + (i << PAGE_SHIFT));

	bio = bio_alloc(bdev, npg, REQ_OP_READ | REQ_SYNC, GFP_KERNEL);
	if (!bio) { rc = -ENOMEM; goto out; }
	bio->bi_iter.bi_sector = disk_off >> 9;
	for (i = 0; i < npg; i++) {
		if (!bio_add_page(bio, hpages[i], PAGE_SIZE, 0)) {
			pr_err("aw_m0_gate: control bio_add_page failed at %u\n", i);
			bio_put(bio);
			rc = -EIO;
			goto out;
		}
	}
	rc = submit_bio_wait(bio);
	bio_put(bio);
	pr_info("aw_m0_gate: control host read = %d\n", rc);
	if (rc) goto out;
	crc_host = crc32(0, hbuf, xfer);

	/* --- verdict --- */
	pr_info("aw_m0_gate: crc_poison=%#x crc_vram=%#x crc_host=%#x\n",
		crc_poison, crc_vram, crc_host);
	if (crc_vram == crc_host && crc_vram != crc_poison)
		pr_info("aw_m0_gate: RESULT=PASS -- disk bytes landed in Arc VRAM by controller DMA\n");
	else if (crc_vram == crc_poison)
		pr_err("aw_m0_gate: RESULT=FAIL -- VRAM still poisoned; nothing was written there\n");
	else
		pr_err("aw_m0_gate: RESULT=FAIL -- VRAM content does not match disk\n");

	vfree(hbuf);
	rc = -EAGAIN;	/* query-only module; self-unload */
out:
	kfree(hpages);
	if (sgl) pci_p2pmem_free_sgl(arc, sgl);
	if (bf) fput(bf);
	if (nvme) pci_dev_put(nvme);
	if (arc) pci_dev_put(arc);
	return rc ? rc : -EAGAIN;
}

module_init(awm0_init);
