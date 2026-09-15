# arcwell — kernel module sources

Goal of this project is DMA from NVMe to GPU VRAM of Intel Arc for efficient LLM
weight streaming. No host RAM buffer — except for housekeeping of necessary
metadata — is allowed.

At 0.0.1 that is built and measured on an Arc Pro B60. See `../results/` for the
raw numbers and `../docs/USING_ARCWELL.md` before building against the API.

## What builds here

`make -C /lib/modules/$(uname -r)/build M=$(PWD)/src modules`

| Object | Role |
|---|---|
| `arcwell.ko` | **the product.** Misc char device `/dev/arcwell`, seven ioctls in `include/aw_uapi.h` |
| `aw_dma_map_probe.ko` | proves `dma_map_resource()` on the GPU BAR returns a usable IOMMU address |
| `aw_p2p_provider_probe.ko` | proves `pci_p2pdma_add_resource()` on Arc BAR2 returns 0 (disproof of the "B1 deadlock") |
| `aw_m0_gate.ko` | the M0 acceptance cell — red-first landing of controller-DMA bytes in VRAM |

The two probes and the gate are kept because they are the evidence for claims
made in `../KERNEL_FACTS.md`. They are not part of the data path.

## How the data path works

1. A VRAM BO is created on xe with host-visible placement (WC +
   `NEEDS_VISIBLE_VRAM` + 64K-aligned size).
2. `AW_IOC_MAP_BUFFER` imports its dma-buf, recovers the BAR physical address,
   and builds `struct page`s over it from the module's `p2pdma` carve.
3. `AW_IOC_READ_BLOCKS` / `AW_IOC_READ_BATCH` build bios directly against those
   pages and `submit_bio()` them at the block device. The NVMe controller writes
   into the GPU BAR. Nothing is staged in host RAM.
4. `AW_IOC_STATS.via_host_bounce` must stay 0. If it is nonzero the transfer did
   not satisfy the no-bounce contract and the caller must treat it as a failure,
   not a slow success.

## Precondition

NVMe controller and GPU must be reachable for P2P — `pci_p2pdma_distance()`
decides, and the module refuses the map if it says no. In practice: same root
complex, or an IOMMU that will translate between them.

## Tests

`test/` holds the acceptance cells. Each is standalone C against
`include/aw_uapi.h`; each is red-first (it is made to fail before it is trusted
to pass). `tools/aw_fiemap.c` translates a file extent to an LBA range.
