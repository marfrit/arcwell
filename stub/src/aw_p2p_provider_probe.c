// SPDX-License-Identifier: GPL-2.0
/* aw_p2p_provider_probe — BACKLOG item 13.
 *
 * Tests the kernel-7.0 p2pdma API against Arc BAR2, using the REAL headers.
 * NO LOCALLY DECLARED EXTERNS. The local-extern workaround in
 * stub/src/arcwell_p2p*.c is defect D4: those files declare
 *
 *   pci_p2pdma_add_resource(pdev, bar, size_t offset, size_t size, int type)
 *
 * but this kernel's include/linux/pci-p2pdma.h declares
 *
 *   pci_p2pdma_add_resource(pdev, bar, size_t size, u64 offset)
 *
 * Five args versus four, with size and offset TRANSPOSED. A call written as
 * (pdev, 2, bar_off, size, 0) therefore reaches the kernel as
 * size = bar_off (multiple GiB) and offset = size. That asks memremap_pages()
 * to build struct pages for a window the caller never intended, which is a
 * plausible cause of the "B1 deadlock" recorded in KERNEL_FACTS.md as a
 * hardware-level limitation. B1 may be an ABI bug, not a kernel property.
 *
 * Also note: KERNEL_FACTS.md says the installed headers carry CONFIG_PCI_P2PDMA=n
 * stubs. On THIS kernel that is false -- /boot/config has CONFIG_PCI_P2PDMA=y and
 * the header carries the real prototypes. The workaround is both unnecessary and
 * the source of the bug.
 *
 * SAFETY: pci_p2pdma_add_resource() is OFF by default (add_resource=0). With it
 * off this module only queries -- pcim_p2pdma_init(), pcim_p2pdma_provider() and
 * pci_p2pdma_map_type(). It allocates no ZONE_DEVICE memory and never calls
 * pci_p2pmem_alloc_sgl(), which KERNEL_FACTS.md records as having wedged the box.
 *
 * Load: insmod aw_p2p_provider_probe.ko arc_bdf=0000:06:00.0 nvme_bdf=0000:01:00.0
 *       [add_resource=1 size=0x4000000 offset=0x200000000]
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/pci-p2pdma.h>
#include <linux/dma-mapping.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("arcwell: kernel-7.0 p2pdma provider probe on Arc BAR2");

static char *arc_bdf = "";
module_param(arc_bdf, charp, 0444);
static char *nvme_bdf = "";
module_param(nvme_bdf, charp, 0444);
static int bar_idx = 2;
module_param(bar_idx, int, 0444);

/* OFF by default. This is the call historically blamed for B1. */
static bool add_resource;
module_param(add_resource, bool, 0444);
MODULE_PARM_DESC(add_resource, "also call pci_p2pdma_add_resource() (default off)");

static unsigned long long size = 0x4000000ULL;		/* 64 MiB */
module_param(size, ullong, 0444);
static unsigned long long offset = 0x200000000ULL;	/* 8 GiB into BAR2 */
module_param(offset, ullong, 0444);

static const char __maybe_unused *map_type_str(enum pci_p2pdma_map_type t)
{
	switch (t) {
	case PCI_P2PDMA_MAP_UNKNOWN:		return "UNKNOWN";
	case PCI_P2PDMA_MAP_NONE:		return "NONE (not a P2PDMA transfer)";
	case PCI_P2PDMA_MAP_NOT_SUPPORTED:	return "NOT_SUPPORTED (host bridge not on allowlist)";
	case PCI_P2PDMA_MAP_BUS_ADDR:		return "BUS_ADDR (switch-local; program PCI bus addresses)";
	case PCI_P2PDMA_MAP_THRU_HOST_BRIDGE:	return "THRU_HOST_BRIDGE (allowlisted; use a normal dma_map_* IOVA)";
	default:				return "??";
	}
}

static struct pci_dev *find_by_bdf(const char *s)
{
	unsigned int dom, bus, slot, fn;

	if (sscanf(s, "%x:%x:%x.%x", &dom, &bus, &slot, &fn) != 4)
		return NULL;
	return pci_get_domain_bus_and_slot(dom, bus, PCI_DEVFN(slot, fn));
}

static int __init awp_init(void)
{
	struct pci_dev *arc, *nvme;
	struct p2pdma_provider *prov;
	int rc;

	if (!arc_bdf[0] || !nvme_bdf[0]) {
		pr_err("aw_p2p_provider_probe: arc_bdf= and nvme_bdf= are MANDATORY\n");
		return -EINVAL;
	}
	arc = find_by_bdf(arc_bdf);
	if (!arc)
		return -ENODEV;
	nvme = find_by_bdf(nvme_bdf);
	if (!nvme) {
		pci_dev_put(arc);
		return -ENODEV;
	}

	pr_info("aw_p2p_provider_probe: arc=%s nvme=%s bar=%d\n",
		pci_name(arc), pci_name(nvme), bar_idx);

	/* --- step 1: devres-managed p2pdma init on the provider device --- */
	rc = pcim_p2pdma_init(arc);
	pr_info("aw_p2p_provider_probe: pcim_p2pdma_init() = %d %s\n",
		rc, rc ? "(FAILED)" : "(ok)");

	/* --- step 2: get a provider for the BAR. NOTE: struct p2pdma_provider is
	 * just { owner, bus_offset } -- no ZONE_DEVICE, no struct pages. This is
	 * the lightweight path that pci_p2pdma_add_resource() is NOT. --- */
	prov = pcim_p2pdma_provider(arc, bar_idx);
	if (IS_ERR_OR_NULL(prov)) {
		pr_err("aw_p2p_provider_probe: pcim_p2pdma_provider(bar=%d) = %ld (FAILED)\n",
		       bar_idx, PTR_ERR(prov));
	} else {
		pr_info("aw_p2p_provider_probe: provider ok owner=%s bus_offset=%#llx\n",
			dev_name(prov->owner), prov->bus_offset);

		/* --- step 3: what kind of P2P transfer is arc -> nvme?
		 * NOTE: pci_p2pdma_map_type() is NOT exported to modules on this
		 * kernel (present in kallsyms as T, absent from Module.symvers), so
		 * an out-of-tree module must infer it from the exported
		 * pci_p2pdma_distance(). >= 0 with no PCI switch in the path means
		 * THRU_HOST_BRIDGE, whose documented mapping is a normal dma_map_*
		 * IOVA -- i.e. exactly the dma_map_resource() already proven to
		 * work in aw_dma_map_probe. --- */
		{
			int dist = pci_p2pdma_distance(arc, &nvme->dev, true);
			phys_addr_t p = pci_resource_start(arc, bar_idx) + offset;
			dma_addr_t bus = pci_p2pdma_bus_addr_map(prov, p);

			pr_info("aw_p2p_provider_probe: pci_p2pdma_distance() = %d -> %s\n",
				dist, dist < 0 ? "NOT_SUPPORTED" :
				      "supported (THRU_HOST_BRIDGE for this topology)");
			pr_info("aw_p2p_provider_probe: bus_addr_map(%pa) = %pad "
				"(only valid for BUS_ADDR-type transfers; informational)\n",
				&p, &bus);
		}
	}

	/* --- step 4 (opt-in): the call blamed for B1, with the CORRECT signature --- */
	if (add_resource) {
		pr_warn("aw_p2p_provider_probe: calling pci_p2pdma_add_resource(bar=%d, size=%#llx, offset=%#llx) "
			"-- CORRECT 4-arg signature from the real header\n",
			bar_idx, size, offset);
		rc = pci_p2pdma_add_resource(arc, bar_idx, (size_t)size, (u64)offset);
		pr_warn("aw_p2p_provider_probe: pci_p2pdma_add_resource() = %d %s\n",
			rc, rc ? "(failed)" : "(OK -- B1 was an ABI bug, not a kernel limit)");
	} else {
		pr_info("aw_p2p_provider_probe: add_resource=0, skipping pci_p2pdma_add_resource()\n");
	}

	pci_dev_put(nvme);
	pci_dev_put(arc);
	return -EAGAIN;	/* do not stay loaded; this is a query-only probe */
}

module_init(awp_init);
