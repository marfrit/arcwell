// SPDX-License-Identifier: GPL-2.0
/* aw_dma_map_probe — BACKLOG item 3.
 *
 * QUESTION THIS ANSWERS (and nothing else): does dma_map_resource() on the Arc
 * BAR2 window, for the NVMe controller's struct device, return a usable bus
 * address -- and is that address translated (IOMMU) or identity?
 *
 * WHY THIS AND NOT THE PREVIOUS ATTEMPTS. KERNEL_FACTS.md records B2 as "a
 * BAR-backed dma_addr handed to the NVMe path is refused". Both recorded
 * attempts were different things:
 *   - arcwell_nvme_vram.c goes through the BLOCK LAYER, which needs struct
 *     pages; VRAM has none without ZONE_DEVICE registration (= B1). Circular.
 *   - m3_nvme.c stuffs a RAW PHYSICAL address into a PRP. A PRP is a BUS
 *     address; under VT-d that faults rather than writes.
 * Neither tested dma_map_resource(), which is the kernel's designated primitive
 * for "give me a DMA address for MMIO that has no struct page". It is the direct
 * analog of the prior art's nvidia_p2p_dma_map_pages()
 * (refs/ssd-gpu-dma/module/map.c:304), which likewise yields bus addresses, not
 * pages (module/map.h:32), fed straight into PRPs (include/nvm_cmd.h:65-72).
 *
 * THIS IS A DIAGNOSTIC, NOT AN ACCEPTANCE CELL. It moves no gate on its own.
 * The red-first acceptance cell is BACKLOG item 4: a BAR2 readback of disk bytes
 * the GPU did not write, which must go red when the destination is mutated.
 *
 * It maps and unmaps only. It issues NO I/O and rings no doorbell, so it cannot
 * wedge the controller. Safe to load.
 *
 * STATUS: UNCOMPILED. Authored on a machine with no kernel build headers (this
 * repo's host has none; no reachable build host has any either). Its kernel API
 * usage is therefore UNVERIFIED against the target kernel -- exactly the class of
 * risk that produced defect D2. COMPILE IT FIRST, on the target, and read the
 * warnings, before insmod. In particular re-check against the target tree:
 *   dma_map_resource() / dma_unmap_resource() arity and argument order,
 *   iommu_get_domain_for_dev() availability and struct iommu_domain::type,
 *   pci_get_class() 24-bit class argument convention.
 *
 * Build: make -C /lib/modules/$(uname -r)/build M=$(PWD) modules
 * Load:  insmod aw_dma_map_probe.ko arc_bdf=0000:06:00.0 nvme_bdf=0000:xx:00.0 \
 *        [bar_off=0x200000000] [len=0x10000]
 *
 * BOTH BDFs ARE MANDATORY. The target box carries TWO Arc cards (A770 + B60);
 * auto-detecting "the display device" could map the wrong card's BAR and produce
 * a confident wrong answer. Likewise for multiple NVMe controllers.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/pci-p2pdma.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("arcwell: probe dma_map_resource() on Arc BAR2 for the NVMe device");

static char *arc_bdf = "";
module_param(arc_bdf, charp, 0444);
MODULE_PARM_DESC(arc_bdf, "Arc GPU BDF, e.g. 0000:06:00.0 (MANDATORY)");

static char *nvme_bdf = "";
module_param(nvme_bdf, charp, 0444);
MODULE_PARM_DESC(nvme_bdf, "NVMe controller BDF (MANDATORY)");

static int bar_idx = 2;
module_param(bar_idx, int, 0444);

static unsigned long long bar_off = 0x200000000ULL;	/* 8 GiB into BAR2 */
module_param(bar_off, ullong, 0444);

static unsigned long long len = 0x10000ULL;		/* 64 KiB */
module_param(len, ullong, 0444);

static struct pci_dev *g_nvme;
static dma_addr_t g_dma;
static bool g_mapped;

static struct pci_dev *find_by_bdf(const char *s)
{
	unsigned int dom, bus, slot, fn;

	if (sscanf(s, "%x:%x:%x.%x", &dom, &bus, &slot, &fn) != 4)
		return NULL;
	return pci_get_domain_bus_and_slot(dom, bus, PCI_DEVFN(slot, fn));
}

static int __init awp_init(void)
{
	struct pci_dev *arc = NULL;
	struct iommu_domain *dom;
	phys_addr_t bar_base, target;
	resource_size_t bar_len;
	int rc = 0;

	/* --- locate the Arc GPU. No guessing: two Arc cards live on the target,
	 * and mapping the wrong BAR would give a confident wrong answer. --- */
	if (!arc_bdf[0]) {
		pr_err("aw_dma_map_probe: arc_bdf= is MANDATORY (e.g. 0000:06:00.0). "
		       "Refusing to guess between the A770 and the B60.\n");
		return -EINVAL;
	}
	arc = find_by_bdf(arc_bdf);
	if (!arc) {
		pr_err("aw_dma_map_probe: no PCI device at arc_bdf=%s\n", arc_bdf);
		return -ENODEV;
	}

	/* --- locate the NVMe controller. Also mandatory, same reason. --- */
	if (!nvme_bdf[0]) {
		pr_err("aw_dma_map_probe: nvme_bdf= is MANDATORY. Refusing to guess.\n");
		rc = -EINVAL;
		goto out_arc;
	}
	g_nvme = find_by_bdf(nvme_bdf);
	if (!g_nvme) {
		pr_err("aw_dma_map_probe: no PCI device at nvme_bdf=%s\n", nvme_bdf);
		rc = -ENODEV;
		goto out_arc;
	}

	bar_base = pci_resource_start(arc, bar_idx);
	bar_len  = pci_resource_len(arc, bar_idx);
	target   = bar_base + bar_off;

	pr_info("aw_dma_map_probe: arc  %s BAR%d phys=%pa len=%pa\n",
		pci_name(arc), bar_idx, &bar_base, &bar_len);
	pr_info("aw_dma_map_probe: nvme %s\n", pci_name(g_nvme));

	if (!bar_base || !bar_len) {
		pr_err("aw_dma_map_probe: BAR%d not assigned\n", bar_idx);
		rc = -ENXIO;
		goto out_both;
	}
	if (bar_off + len > bar_len) {
		pr_err("aw_dma_map_probe: bar_off+len (%#llx) exceeds BAR len (%pa)\n",
		       bar_off + len, &bar_len);
		rc = -ERANGE;
		goto out_both;
	}

	/* --- is the NVMe device behind an IOMMU at all? --- */
	dom = iommu_get_domain_for_dev(&g_nvme->dev);
	pr_info("aw_dma_map_probe: nvme iommu domain=%s\n",
		!dom ? "NONE (no IOMMU / passthrough at device level)" :
		dom->type == IOMMU_DOMAIN_IDENTITY ? "IDENTITY (pass-through)" :
		dom->type == IOMMU_DOMAIN_DMA ? "DMA (translating)" : "other");

	/* --- P2P topology. NOTE: pci_p2pdma_distance() is a TOPOLOGY QUERY. It
	 * does NOT require pci_p2pdma_add_resource(), so this does not touch the
	 * deadlocking carve (B1). xe's own attach hook calls exactly this
	 * (xe_dma_buf.c:30) and SILENTLY CLEARS attach->peer2peer if it is < 0,
	 * which then migrates the BO to system RAM -- a host bounce reported as
	 * success. That is defect D3. This line is the early warning. --- */
	{
		int dist = pci_p2pdma_distance(arc, &g_nvme->dev, true);

		pr_info("aw_dma_map_probe: pci_p2pdma_distance(arc -> nvme) = %d %s\n",
			dist,
			dist < 0 ? "<< NEGATIVE: xe would silently disable peer2peer and BOUNCE THROUGH HOST RAM"
				 : "(non-negative: xe will keep attach->peer2peer)");
	}

	/* --- the actual question --- */
	g_dma = dma_map_resource(&g_nvme->dev, target, len, DMA_FROM_DEVICE, 0);
	if (dma_mapping_error(&g_nvme->dev, g_dma)) {
		pr_err("aw_dma_map_probe: RESULT=FAIL dma_map_resource(phys=%pa len=%#llx) returned error\n",
		       &target, len);
		rc = -EIO;
		goto out_both;
	}
	g_mapped = true;

	pr_info("aw_dma_map_probe: RESULT=OK phys=%pa -> dma_addr=%pad len=%#llx\n",
		&target, &g_dma, len);
	pr_info("aw_dma_map_probe: translation=%s\n",
		((u64)g_dma == (u64)target) ? "IDENTITY (dma_addr == phys; a raw PRP would have worked)"
					    : "TRANSLATED (dma_addr != phys; a raw-physical PRP was always going to fault)");
	pr_info("aw_dma_map_probe: dma_mask=%#llx  this address %s the mask\n",
		g_nvme->dev.coherent_dma_mask,
		((u64)g_dma <= g_nvme->dev.coherent_dma_mask) ? "fits" : "EXCEEDS");

	pci_dev_put(arc);
	return 0;	/* stay loaded; mapping held for BACKLOG item 4 */

out_both:
	pci_dev_put(g_nvme);
	g_nvme = NULL;
out_arc:
	pci_dev_put(arc);
	return rc;
}

static void __exit awp_exit(void)
{
	if (g_mapped)
		dma_unmap_resource(&g_nvme->dev, g_dma, len, DMA_FROM_DEVICE, 0);
	if (g_nvme)
		pci_dev_put(g_nvme);
	pr_info("aw_dma_map_probe: unloaded\n");
}

module_init(awp_init);
module_exit(awp_exit);
