# KERNEL FACTS — measured, current, module-relevant

Single authoritative source for what the hardware and kernel actually do.
Every entry was measured on the target box. If something here contradicts your
assumption, this file is right and your assumption is the thing to drop.

## Architecture (not negotiable)

Goal of this project is DMA from NVME to GPU VRAM of Intel Arc A770 and Intel Arc B60 for efficient LLM weight streaming. No host RAM Buffer - except for housekeeping of necessary metadata - is allowed

arcwell is a **kernel module**. The NVMe controller DMAs expert weights into Arc
VRAM; the module's ioctl surface is `stub/include/aw_uapi.h`
(`AW_IOC_MAP_BUFFER`, `AW_IOC_READ_BLOCKS`, `AW_IOC_STATS`).

There is **no userspace data path** and none is to be designed, benchmarked, or
proposed. Reason, once, so it never needs relitigating: a userspace read lands
block-device bytes in host pages before any userspace code can move them. A
userspace "direct storage" path is therefore the host bounce buffer — the thing
the project exists to remove. This was decided at leg 006 after direct
measurement; it is not open.

## The target buffer exists (this is solved)

A host-visible VRAM BO creates and works on the **stock** xe driver. No patch, no
rebuild. Requirements, all four, or `DRM_IOCTL_XE_GEM_CREATE` returns `-EINVAL`:

- `placement = 1 << DRM_XE_MEM_REGION_CLASS_VRAM`
- `flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM`
- `cpu_caching = DRM_XE_GEM_CPU_CACHING_WC` — WC is mandatory for VRAM
- `size` a multiple of **64 KiB** (DG2 flat-CCS sets the VRAM manager
  `min_page_size = 64K`)

Measured on the A770: `VISIBLE VRAM 0x7800000000 size 0x400000000` — the full
16 GiB is host-visible; `mmap`, WC write and readback all succeed.
`BAR2_base = 0x7800000000` (matches `pci_resource_start(dev, 2)` and sysfs).

An earlier "host-visible VRAM is rejected by a driver gate" conclusion was
**wrong**: it was the probe omitting WC and 64K alignment. Do not reopen it. A
patch to `xe_bo.c` that disables the visible-placement check in
`xe_ttm_io_mem_reserve` breaks Arc Pro B60 SA-manager allocation and was
reverted — that route is closed.

## SOLVED 2026-09-15: NVMe controller DMA lands disk bytes in Arc A770 VRAM

The M0 feasibility gate is **passed, red-first**. Raw output:
`results/M0_GATE_2026-09-15.txt`. Cell: `stub/src/aw_m0_gate.c`.

```
GREEN: crc_poison=0x1869d5fa crc_vram=0x1d01c2b9 crc_host=0x1d01c2b9
       RESULT=PASS -- disk bytes landed in Arc VRAM by controller DMA
RED  : crc_poison=0x9f9f4226 crc_vram=0x1f9e697e crc_host=0x1d01c2b9
       RESULT=FAIL -- VRAM content does not match disk
```

The red run mutates the verification address; the cell goes red naming the cell,
so it is not decoration. Supporting facts, all measured the same session:

- `is_p2p=1` on the sg pages: they are genuine ZONE_DEVICE P2PDMA pages carved
  from Arc BAR2 at `phys=0x7a00000000`.
- `blk_queue_pci_p2pdma=1` on the NVMe queue.
- `submit_bio_wait() = 0`.
- Verification reads BAR2 back through `ioremap` + `memcpy_fromio`, a path the
  GPU never wrote, and the 0xA5 poison is gone.
- Control: the same LBA range read into ordinary host pages CRCs identically.
- The block device was opened `BLK_OPEN_READ` and never written.

**Still open:** this lands in raw carved BAR2 p2pmem, not an xe-managed GEM BO, so
OpenCL/OpenVINO cannot yet consume it. Reconciling the carve with a BO is the
next real problem.

## SOLVED 2026-09-15: bytes land in a GPU-OWNED BO, no host bounce

The M0 gate above landed bytes in raw carved BAR2, which no GPU context owned.
This closes that gap. Raw output: `results/BO_GATE_2026-09-15.txt`. Module:
`stub/src/arcwell.c` (serves `aw_uapi.h`); cell: `stub/test/aw_bo_test.c`.

```
GREEN: AW_IOC_MAP_BUFFER ok out_flags=0x1 (REQUIRE_P2P honoured)
       stats: reads=2 bytes=2097152 us=14136 segments=512 via_host_bounce=0
       RESULT=PASS -- disk bytes landed in a GPU-owned BO, verified through its xe mapping
RED  : control read from 0x1000100000  <-- MUTATED COMPARAND
       RESULT=FAIL -- BO content does not match disk
```

Verification reads the BO back through **its own xe mmap**, not a raw BAR window,
so the buffer is one the GPU owns. `via_host_bounce=0`.

**The working recipe, no xe patch required:**

1. Userspace creates the BO (WC + `NEEDS_VISIBLE_VRAM` + 64K-aligned) and exports
   it with `DRM_IOCTL_PRIME_HANDLE_TO_FD`.
2. Kernel: `dma_buf_dynamic_attach()` with `.allow_peer2peer = true`. **Never
   `dma_buf_attach()`** — it leaves `peer2peer` false and xe then migrates the BO
   to system RAM, giving a host bounce that reports success (defect D3).
3. **Re-read `attach->peer2peer` after attaching and fail if false.** xe clears it
   silently when `pci_p2pdma_distance() < 0` (`xe_dma_buf.c:30-32`).
4. `dma_buf_pin()` + `dma_buf_map_attachment()` under `dma_resv_lock()`.
   Entries are page-less; only `sg_dma_address()` is meaningful.
5. `iommu_iova_to_phys(iommu_get_domain_for_dev(&nvme->dev), sg_dma_address(sg))`
   recovers the BAR2 physical address. Required here: the IOMMU translates.
6. `pci_p2pdma_add_resource()` that range (2 MiB aligned) so ZONE_DEVICE pages
   exist, then `pfn_to_page()`; assert `is_pci_p2pdma_page()`.
7. Ordinary bio + `submit_bio_wait()`. nvme handles the P2P mapping itself.

Modules using dma-buf must `MODULE_IMPORT_NS("DMA_BUF")`.

**Known limits of this result, not yet addressed:** one 1 MiB BO, single sg
segment; the per-BO carve is never released (devres, until device rebind), so many
registrations will exhaust BAR2; 2 MiB alignment over-carves; and **no OpenCL or
OpenVINO kernel has yet read the buffer** — verification was a CPU read through
the BO's mapping. `ENGINEER_PROMPT.md` §2 requires the consumption path be proven.

## A CARVED BAR AND GPU RUNTIME SUSPEND DO NOT MIX — this wedges the card

Measured 2026-09-15, the hard way, on the A770.

`pci_p2pdma_add_resource()` on BAR2 creates ZONE_DEVICE pages backed by that BAR.
If xe then attempts a runtime suspend, the transition times out:

```
xe 0000:06:00.0: [drm] *ERROR* Tile0: GT0: runtime suspend failed (-ETIMEDOUT)
xe 0000:06:00.0: can't suspend (xe_pci_runtime_suspend [xe] returned -110)
xe 0000:06:00.0: Runtime PM usage count underflow!
```

The device latches into `power/runtime_status=error` and from that point **every
`DRM_IOCTL_XE_GEM_CREATE` on it returns `-EINVAL`, for every process on the
machine** — inside a container, inside a chroot, on the host, all the same. It
does NOT recover from `echo on > power/control`; that only bumps `runtime_usage`.
Recovery requires a driver rebind or a reboot.

**Mitigation, implemented in `stub/src/arcwell.c`:** take
`pm_runtime_get_sync(&gpu->dev)` at module load and `pm_runtime_put()` at unload,
so the suspend is never attempted while a carve exists. The B60 ran the full
end-to-end cell with this in place and stayed `runtime_status=active`.

**Residual hazard:** carves are devres-owned by the `pci_dev` and outlive the
module. After `rmmod` the BAR is still carved and nothing holds the GPU awake. Do
not leave a carved GPU idle with arcwell unloaded until carve release exists.

## END TO END: NVMe bytes reach an OpenCL kernel, no host bounce

Raw output: `results/E2E_2026-09-15.txt`. One BO, one dma-buf fd, both halves.

```
cpu_sum=0x928a26d1 gpu_sum=0x928a26d1 expect=0x928a26d1
stats: reads=2 bytes=2097152 us=2305 segments=512 via_host_bounce=0
RESULT=PASS -- NVMe bytes reached an OpenCL kernel on Intel(R) Arc(TM) Pro B60 Graphics with no host bounce
```

`expect` is computed on the host from the raw block device, so the bytes the GPU
summed are provably the disk's. Completion is fenced on a `cl_event`, per
`ENGINEER_PROMPT.md` §2. The mutated run reports FAIL.

OpenCL imports the BO with `cl_khr_external_memory_dma_buf`
(`CL_EXTERNAL_MEMORY_HANDLE_DMA_BUF_KHR`, 0x2067). **`clinfo`'s extension string
lists only `cl_khr_external_memory` and NOT the dma_buf sub-extension — reading it
would give a false negative.** Query
`CL_DEVICE_EXTERNAL_MEMORY_IMPORT_HANDLE_TYPES_KHR` instead; both cards return
0x2067.

## Both cards work. B60 parity confirmed 2026-09-15

Raw output: `results/B60_PARITY_2026-09-15.txt`. The A770 and the Arc Pro B60
(Battlemage G21 `[8086:e211]`, `0000:0f:00.0`, BAR2 `0x6000000000` +32 GiB) each
pass the M0 gate and the BO gate, red-first, with `via_host_bounce=0`.

Two corrections to earlier notes in this campaign:

- **IOMMU group sharing is not required.** The B60 is in group 2 while the NVMe is
  in group 0, and `dma_map_resource()` works anyway — it maps into the *NVMe
  device's* domain regardless of the provider's group. An earlier note treated
  shared group 0 as a reason to prefer the A770; that reason does not hold.
- `pci_p2pdma_distance()` is **6** for the B60 versus **8** for the A770.

## B1 and B2 were both DISPROVED on 2026-09-15 — read this before citing them

The two entries below were wrong. Both blockers were **self-inflicted ABI bugs in
locally-declared externs**, not properties of the hardware or the kernel.

**B1 (`pci_p2pdma_add_resource` "deadlocks on Arc BAR2").** The probes declared it
with 5 parameters and `size`/`offset` transposed; this kernel exports 4. A call
meant as `(pdev, 2, offset, size, 0)` arrived as `size = offset` — multiple GiB —
so `memremap_pages()` was asked to build struct pages for a window nobody
intended. With the real prototype from the real header, same hardware, xe loaded:
`pci_p2pdma_add_resource() = 0` in **9 ms**, and the kernel logged
`xe 0000:06:00.0: added peer-to-peer DMA memory 0x7a00000000-0x7a03ffffff`.

**B2 ("a BAR-backed dma_addr is refused by the NVMe path").** Never actually
tested. `dma_map_resource()` on Arc BAR2 for the NVMe device returns a valid
IOMMU-translated address: `phys=0x7a00000000 -> dma_addr=0xff980000`.

**The stale-header claim below is also false on this kernel.**
`/boot/config-7.0.14-12-pve` has `CONFIG_PCI_P2PDMA=y` and
`include/linux/pci-p2pdma.h` carries the real prototypes. The local-extern
workaround that caused B1 was never necessary.

Correct signatures on this kernel, for anyone tempted to declare them again:
```c
int  pci_p2pdma_add_resource(struct pci_dev *pdev, int bar, size_t size, u64 offset);
struct scatterlist *pci_p2pmem_alloc_sgl(struct pci_dev *pdev, unsigned int *nents, u32 length);
int  pcim_p2pdma_init(struct pci_dev *pdev);
struct p2pdma_provider *pcim_p2pdma_provider(struct pci_dev *pdev, int bar);
```
`pci_p2pdma_map_type()` is in kallsyms but **not** in `Module.symvers`; modules
must infer the map type from the exported `pci_p2pdma_distance()`.

A carve persists on the `pci_dev` (devres) until the device is rebound. Re-adding
an overlapping range trips `Conflicting mapping in same section`
(`mm/memremap.c:158`) and returns `-ENOMEM`. Sections are 128 MiB on x86, so even
a different offset inside the same section conflicts. Probe with
`pci_p2pmem_alloc_sgl()` first and carve only if it returns NULL.

## SUPERSEDED (kept for provenance): the gate: the NVMe side will not map BAR memory

`B2` is the remaining problem. A BAR-backed `dma_addr` handed to the NVMe path is
refused: the driver maps bio pages through the DMA API, and a BAR address with no
P2P registration is not accepted. A raw submit that stuffs a BAR address into a
PRP produced a kernel WARN (`insmod exited with irqs disabled`) — and note that a
sync submit from `module_init` is itself an unsafe context; do it from a
workqueue.

`B1` blocks the sanctioned route to `B2`: `pci_p2pdma_add_resource()` on Arc
BAR2 **deadlocks** post-reboot. Measured across every variation tried — offset 0
and 8 GiB, size 1 MiB/16 MiB/1 GiB/4 GiB, with xe loaded and with xe blacklisted
via `install xe /bin/true`. It blocks (module stuck `Loading`, low load, not
spinning); killing `insmod` does not stop `module_init`; the module cannot be
removed. The pre-reboot `rc=0` carve was a transient state, not a reproducible
result.

Never call `pci_p2pmem_alloc_sgl()` with a 1M-entry / multi-GiB request: it
wedged the box and required a hardware reboot. Carve once, then
alloc/stream/free in 16 MiB chunks.

Note the asymmetry that makes `B2` tractable: the **mechanism** is present. The
running kernel has `CONFIG_PCI_P2PDMA=y`, P2P distance NVMe↔GPU = 8, and in a
pre-reboot state a bio containing BAR pages was accepted and `submit_bio_wait()`
completed with no IOMMU fault. What is missing is a mapping the NVMe driver will
accept, not hardware capability.

## Stale headers are a trap that produces confident wrong answers

`/usr/src/linux-headers-$(uname -r)` had `CONFIG_PCI_P2PDMA=n` stubs while the
running kernel had it **enabled** (29 `pci_p2pdma*` symbols in `/proc/kallsyms`).
Compiling against the headers yields a false "capability absent" verdict.

**Never trust a header or a remembered constant over the running kernel.** Same
shape of failure recurred with a hardcoded `FS_IOC_FIEMAP`: the remembered
`0xc020940b` gave `ENOTTY` and a false "no extents"; the header says `0xc020660b`
(type `'f'`). Derive such constants from the header and cross-check.

## THE ext4 EXISTS — and the device is shared with ZFS SLOG and L2ARC

Measured 2026-09-15, after the operator created it.

```
nvme0n1p1   16 GiB  PARTLABEL rpool-slog   zfs_member   <- rpool LOG vdev
nvme0n1p2   32 GiB  PARTLABEL rpool-l2arc  zfs_member   <- rpool CACHE vdev
nvme0n1p3  190.5GiB PARTLABEL flash        ext4 "flash" -> /flash (rw,noatime)
```

**The FIEMAP offset is now a known constant.** `nvme0n1p3` starts at sector
**100665344** (51540656128 B = 48.0 GiB). `fe_physical` from `FS_IOC_FIEMAP` is
filesystem-relative, so absolute LBA = `fe_physical/512 + 100665344`. Getting this
wrong reads someone else's data, silently.

**MEASUREMENT HAZARD: the expert store shares its device with rpool's log and
cache vdevs.** `zpool status rpool` confirms `...part1` as the `logs` vdev and
`...part2` as `cache`. Every synchronous write anywhere on rpool hits the SLOG on
this NVMe, and L2ARC traffic hits p2 — both contending with expert reads on p3,
on the same controller and the same PCIe Gen3 x4 link. All the containers live on
rpool, so "host quiet" for a storage leg means **guest-quiet too**, not merely no
GPU users. Any `B_P` number taken while rpool is active is measuring contention.

**A comparand that was valid this morning is now void.** The raw offset
`0x1000000000` used by earlier cells in this campaign lies 16.0 GiB inside the new
ext4; `mkfs` zeroed it. It now reads word-sum `0x00000000`, 0 nonzero bytes of
1 MiB, against the `0x928a26d1` the cells were given. Re-derive every comparand
against the current on-disk state before reusing a cell.

## SUPERSEDED — the decision that led to the above

## RESOLVED 2026-09-15: expert weights go on a dedicated ext4

Operator decision, taken after this campaign re-confirmed that neither filesystem
in the fleet can translate a file to block ranges: **if ZFS and btrfs do not
work, create an ext4.** That unblocks the file-addressed route in `M4_API.md`,
which was otherwise unimplementable on this hardware.

ext4 supplies a real `FS_IOC_FIEMAP` and is not copy-on-write, so extents are
stable for a read-only weight file. That makes `M4_API.md` option (B) — one file
per expert — work as intended: `fallocate` can lay an expert down as a single
contiguous extent, i.e. one DMA segment rather than a gather list, and
`AW_READ_F_REVALIDATE` / `-ESTALE` becomes a cheap safety check instead of a
standing hazard.

Three things that must be got right when implementing the translation:

- **FIEMAP is filesystem-relative.** `fe_physical` is an offset within the
  filesystem, so for an ext4 on `nvme0n1pN` the partition start sector must be
  added to reach the absolute LBAs `AW_IOC_READ_BLOCKS` takes. Put the filesystem
  on a raw partition; LVM/dm or a zvol underneath adds another translation layer
  that FIEMAP does not account for.
- **Reject `FIEMAP_EXTENT_UNWRITTEN`, `_DELALLOC`, `_INLINE` and `_ENCODED`**, and
  `fsync()` before mapping. An unwritten or delayed-allocation extent does not yet
  hold the bytes, and DMAing from it reads stale disk content.
- **Derive the ioctl number from the header.** See the stale-header section below:
  a remembered `FS_IOC_FIEMAP` of `0xc020940b` returned `ENOTTY` and produced a
  false "no extents" verdict; the header's value is `0xc020660b`.

Per `M4_API.md` the translation is NOT in the module: the ioctls take raw block
ranges, and FIEMAP is a once-at-open step performed by whoever opens the file.

## Extents: neither filesystem in the fleet provides them (SUPERSEDED — see above)

- The target NVMe carries **ZFS** — no FIEMAP, no contiguous-allocation API, and
  being COW it can relocate blocks on rewrite or snapshot.
- btrfs returns `EOPNOTSUPP` for FIEMAP on the build host.

So: physical contiguity is never assumed; gather with SGL/PRP and make the
segment count observable. A cached block map must be validated, and a moved map
must return `-ESTALE` rather than read wrong bytes. FIEMAP, if used at all, is an
open-time translation only.

## Consumption path

SYCL is dead under xe (memcpy abort). Vulkan is slow on Battlemage. **OpenCL via
OpenVINO** is the only measurably fast path on both cards; fence completion on an
OpenCL/OpenVINO event, not Level Zero.

## Prior art

`refs/ssd-gpu-dma` (BSD-2-Clause) is a Linux **kernel module** (`module/ctrl.c`)
plus a userspace client. The port target is the module's
NVMe-controller-DMA-into-GPU-memory wiring and its P2P device mapping
(`nvm_dis_ctrl_map_p2p_device`, `nvm_dma_map`). The userspace client is only the
ioctl caller. It is A770-only and demo-grade; learn the wiring, do not fork it.
It is NVIDIA-GPUDirect-lineaged: there is **no** source-verified Intel Arc
NVMe→VRAM P2P demonstration, and NVIDIA GPUDirect results are not evidence about
Intel Arc.

## MEASURED NEGATIVE: xe.probe_display=0 reclaims NO VRAM on a headless box

Tried 2026-09-15 on a machine with no connected outputs on either card. The
parameter is real and it works — `xe_module.c:56`,
`module_param_named(probe_display, ..., bool, 0444)` — and with `probe_display=N`
the display IP is genuinely left alone: the `discrete display version` probe and
the DMC firmware load both disappear from the boot log.

**It frees nothing.** Global memory is byte-identical before and after:

| card | before | after |
|---|---|---|
| Arc Pro B60 | 24385683456 (22.71 GiB) | 24385683456 (22.71 GiB) |
| Arc A770 | 16225243136 (15.11 GiB) | 16225243136 (15.11 GiB) |

So the gap between nameplate and usable is NOT display. Where it actually goes:

```
A770  16.000 GiB physical
    - 0.094 GiB stolen   -> 15.906 GiB  xe "usable size exclude stolen"
    - 0.795 GiB ???      -> 15.111 GiB  OpenCL CL_DEVICE_GLOBAL_MEM_SIZE
B60   24.000 -> 23.906 -> 22.711        (1.195 GiB unaccounted)
```

~810 MiB (A770) and ~1224 MiB (B60) vanish *below* xe's own usable figure, and the
amount scales with card size rather than being a fixed display cost — consistent
with GuC/HuC images, GGTT page tables and TTM reservations. Identifying which
needs instrumentation beyond dmesg; not done.

Also recorded while measuring this: the **A770's maximum single OpenCL allocation
is 4 GiB** (`4294959104`) against the B60's full 22.71 GiB. That bounds any single
buffer on that card regardless of total free VRAM.

Keep or drop the parameter on non-memory grounds: it removes the display engine
from a card whose BAR is being carved, which is one less subsystem with a claim on
VRAM the carve might overlap. It buys no capacity.

## THE A770's PCIe PATH GOES UNRESPONSIVE. CAUSE NOT ESTABLISHED.

**This section previously read "the A770 cannot carry a p2pdma carve under load".
That was an over-claim and is withdrawn.** Both cards carried an identical
overrunning carve; only the A770 failed, and it is the one behind a switch chain.
Nothing measured ties the failure to the carve.

What IS established, from netconsole and this kernel's own xe source:

```
xe_ggtt.c:519 = xe_gt_WARN(gt, err, "Failed to invalidate GGTT")
1074.57  WARN in xe_ggtt_remove_bo <- xe_ttm_bo_destroy <- xe_lrc_destroy <- exec queue teardown
1078.48  GT0: Pending enable/disable failed to respond      (GuC already not answering)
1078.48  GT0: reset started
1078.68  Force wake domains 0,1,3,5,11,12: MMIO unreliable (forcewake returns 0xFFFFFFFF)
1079.69  reset failed (-ETIMEDOUT) -> CRITICAL: Xe has declared device as wedged
```

Read it in order. The GGTT WARN is a **symptom** — GGTT invalidation goes through
the GuC, so it failing means GuC comms were already dead. Underneath everything is
`0xFFFFFFFF` from forcewake registers, and an all-ones MMIO read is not a
GPU-internal fault: **the PCIe read got no response.** The device stopped
answering on the bus, and the reset then could not complete.

Three independent observations point the same way, at the bus rather than at
arcwell:

- the A770 hangs off the B550 chipset behind two Intel switch bridges
  (`8086:4fa0` / `8086:4fa4`); the B60 is on a direct CPU Gen4 x8 port and has
  never done this, under identical code and an identical carve;
- `B_FABRIC` on the A770 measured **1.84 GB/s, 47% of its own Gen3 x4 link**,
  against the B60's 89% of Gen4 x8. That path underperformed before any wedge;
- the shutdown hang was `pcieport 0000:05:04.0: Unable to change power state from
  D3cold to D0, device inaccessible` — a *port* in that same chain, not the GPU.

**Known and fixed, separately:** two of the three wedges had a different and
identified cause — `rmmod` dropped a module-lifetime PM reference while the BAR
was carved, and a carved BAR plus a D3 transition wedges the card. That is fixed
with `pm_runtime_forbid()` and verified to survive `rmmod`. The third wedge
happened with that fix active and is the unexplained one.

**THE DECIDING CELL HAS NOT BEEN RUN:** sustained GPU load on the A770 with **no
carve and arcwell not loaded**. If it wedges, the board is the cause and arcwell is
a bystander; if it does not, the carve is implicated. Until that runs, neither
statement is verified — and a negative that was never tried is not a negative.

**Operator decision 2026-09-15: the A770 is backlogged.** All further arcwell work
is on the B60. Practical consequence regardless of cause: the card is behind a
path that both underperforms and drops off the bus, and once it wedges the
processes touching it cannot be killed and the box needs a power cycle.

## MEASURED NEGATIVE: xe.probe_display=0 reclaims NO VRAM on a headless box

Tried 2026-09-15 on a machine with no connected outputs on either card. The
parameter is real and it works — `xe_module.c:56`,
`module_param_named(probe_display, ..., bool, 0444)` — and with `probe_display=N`
the display IP is genuinely left alone: the `discrete display version` probe and
the DMC firmware load both disappear from the boot log.

**It frees nothing.** Global memory is byte-identical before and after:

| card | before | after |
|---|---|---|
| Arc Pro B60 | 24385683456 (22.71 GiB) | 24385683456 (22.71 GiB) |
| Arc A770 | 16225243136 (15.11 GiB) | 16225243136 (15.11 GiB) |

So the gap between nameplate and usable is NOT display. Where it actually goes:

```
A770  16.000 GiB physical
    - 0.094 GiB stolen   -> 15.906 GiB  xe "usable size exclude stolen"
    - 0.795 GiB ???      -> 15.111 GiB  OpenCL CL_DEVICE_GLOBAL_MEM_SIZE
B60   24.000 -> 23.906 -> 22.711        (1.195 GiB unaccounted)
```

~810 MiB (A770) and ~1224 MiB (B60) vanish *below* xe's own usable figure, and the
amount scales with card size rather than being a fixed display cost — consistent
with GuC/HuC images, GGTT page tables and TTM reservations. Identifying which
needs instrumentation beyond dmesg; not done.

Also recorded while measuring this: the **A770's maximum single OpenCL allocation
is 4 GiB** (`4294959104`) against the B60's full 22.71 GiB. That bounds any single
buffer on that card regardless of total free VRAM.

Keep or drop the parameter on non-memory grounds: it removes the display engine
from a card whose BAR is being carved, which is one less subsystem with a claim on
VRAM the carve might overlap. It buys no capacity.

## THE A770 CANNOT CARRY A p2pdma CARVE UNDER LOAD. THE B60 CAN.

Measured three times on 2026-09-15, twice by accident and once while deliberately
running the same leg on both cards.

| | A770 `06:00.0` | B60 `0f:00.0` |
|---|---|---|
| path | B550 chipset -> 2 Intel switch bridges -> Gen3 x4 | CPU root port, Gen4 x8 |
| `B_FABRIC` | 1.84 GB/s | 13.99 GB/s |
| carve + arcwell DMA | works | works |
| carve + **GPU work on the same card** | **wedges** | fine at 384 MiB carved |

The failure signature is identical every time:

```
xe 0000:06:00.0: [drm] *ERROR* Tile0: GT0: CT write: non-zero status: 4294967295
WARNING: xe_ggtt.c:519 at ggtt_invalidate_gt_tlb ... aw_bp/<pid>   (repeating)
-> GT reset fails -ETIMEDOUT -> "Xe has declared device ... as wedged"
-> every GEM_CREATE returns -ECANCELED thereafter
```

`0xFFFFFFFF` from a forcewake or GuC CT register means MMIO to the card is gone.
Once that happens **the processes touching it cannot be killed**: xe retries the
GGTT TLB invalidation forever and `SIGKILL` does not reap them, so the box needs a
reboot rather than a cleanup.

**The B60 ran the identical leg with a 384 MiB carve and stayed healthy**, so this
is not "p2pdma carves are unsafe". It is specific to the A770's path on this
board. Hypothesis, NOT asserted: P2P/MMIO through the chipset plus that switch
chain is marginal, and carving the BAR pushes it past what it will sustain.

**Consequence:** the A770 is not merely ineligible on bandwidth (its `B_FABRIC`
sits below the drive's own rate) — it is **unusable for arcwell work on this
hardware**. Do not carve it. The CPU-cost pair the campaign wanted from that card
cannot be taken on it.

**And a warm reboot may not be enough.** A `systemctl reboot` of a box whose A770
is wedged hung in `systemd-shutdown` on 2026-09-15 with the hardware watchdog
failing to stop, TSC marked unstable, a CPU soft lockup and the NIC's transmit
queue timing out; it took an operator power cycle at the smart plug. Warm reboots
do not reset a PCIe device.

## A p2pdma CARVE IS NOT AN ALLOCATION. It cannot be released; it also holds nothing.

**Read this before concluding anything about weights.** A carve registers BAR
*address ranges* so the kernel has `struct page`s describing them. It allocates no
VRAM, holds no data, pins nothing, and blocks no allocation. Weights load and
unload normally with a carve present; arcint starts and stops normally; arcwell
itself unloads and reloads.

Demonstrated 2026-09-15 on a B60 with 160 MiB carved: three consecutive runs each
registered 8 buffers, unmapped 4 and closed with 4, all passing, and the **same
physical VRAM addresses recurred in all three runs** — which only happens if the
previous buffers were genuinely freed and the memory handed out again. `rmmod` and
reload of arcwell with the carve still present: clean, batch cell passes.

| thing | released when |
|---|---|
| VRAM holding weights (BOs) | on free / process exit — normally |
| arcwell buffer registrations | `AW_IOC_UNMAP_BUFFER`, or fd close |
| the arcwell module | `rmmod`, any time |
| **the carve** (struct pages over BAR ranges) | only on GPU unbind or reboot |

So the carve's cost is **host RAM only**, and nothing else.

`pci_p2pdma_add_resource()` has **no counterpart** in this kernel — no remove, no
release, no free. The registration is devres-owned by the `pci_dev` and goes away
only when the device is unbound. Measured by inspection of
`include/linux/pci-p2pdma.h`: nothing matching `remove|release|free|destroy|fini`.

**This is bounded, not a leak.** Incremental carving only grows, but it converges:
once all usable VRAM is registered no further carve can occur. The ceiling is
struct pages for the carved range, ~64 bytes per 4 KiB page:

| card | usable VRAM | struct pages | host RAM if fully carved |
|---|---|---|---|
| Arc Pro B60 | 23.906 GiB | 6,266,814 | ~382 MiB (1.56% of VRAM) |
| Arc A770 | 15.906 GiB | 4,169,662 | ~254 MiB (1.56% of VRAM) |

1.56% of VRAM in host RAM, as housekeeping metadata rather than a data buffer.

**Consequence for design:** paying it up front beats growing into it. A single
carve of all usable VRAM at module load is deterministic, removes the granule
walk-down, the in-section conflict handling and the `-ENOSPC` retries, and
guarantees every BO xe can hand out is already covered. Implemented as
`carve_all=1` in `stub/src/arcwell.c`.

**But `carve_all` requires a BAR with NO existing carves.** Measured 2026-09-15:
with 160 MiB already carved on the B60 from earlier legs in the same boot, a
full-VRAM carve returns `-ENOMEM` in 13 ms — far too fast to be the 382 MiB
allocation failing with 56 GiB free, and no `Conflicting mapping` WARN. It fails
early on the overlap. Since carves cannot be released, that means **`carve_all`
must be the first carve after boot or device rebind**, or not at all. Untested on
a clean BAR; needs a fresh-boot window.

## A BIO HOLDS 256 VECTORS. MORE IS NOT AN ERROR, IT IS A KERNEL BUG()

Measured 2026-09-15, the hard way, four times.

`BIO_MAX_VECS` is **256U** (`include/linux/bio.h:13`). `bio_alloc()` with a
larger `nr_vecs` does **not** return NULL — it reaches `default: BUG();` in
`biovec_slab()`, `block/bio.c:61`:

```
kernel BUG at block/bio.c:61!
Oops: invalid opcode: 0000 [#1] SMP NOPTI
Comm: aw_bp Tainted: P U OE 7.0.14-12-pve
```

A page is 4 KiB, so **one bio covers at most 1 MiB**. A 2,457,600-byte expert
slice is 600 pages and BUGs the kernel. The length comes from userspace via
`AW_IOC_READ_BLOCKS`, so this was a kernel-crashing defect reachable from an
ioctl argument.

**Why it stayed hidden:** every cell in this campaign until now used a 1 MiB
transfer — exactly 256 pages, sitting precisely on the limit without crossing it.
The defect was present in the first version of `stub/src/arcwell.c` and only
surfaced the first time a realistic expert size was used. A test size chosen for
convenience masked it.

**Fix:** `aw_submit_request()` splits any request into bios of at most
`BIO_MAX_VECS` pages, advancing the LBA by `done << (PAGE_SHIFT - 9)` per chunk,
with every bio counted in the shared completion structure. Both the single-read
and batch paths go through it, so `out_segments` now reports real bios rather
than page count.

**Anything measured on a kernel that has Oopsed is suspect.** The taint word read
12481 with `[D]=DIE` after this; the box was rebooted before any gate number was
taken.

## THE PCIe FABRIC: the GPUs are NOT on a CPU x16 slot

Measured 2026-09-15 from the kernel's own view (`current_link_speed` /
`current_link_width` on the CPU root ports, which report reliably).

| CPU root port | link | rate | feeds |
|---|---|---|---|
| `00:01.1` | Gen3 x4 (8.0 GT/s x4) | ~3.94 GB/s | NVMe `01:00.0` |
| `00:01.2` | Gen3 x4 (8.0 GT/s x4) | ~3.94 GB/s | **A770 `06:00.0`** |
| `00:03.1` | Gen4 x8 (16.0 GT/s x8) | ~15.75 GB/s | **B60 `0f:00.0`** |

The A770 sits behind the **B550 chipset**, not a CPU x16 slot:
`00:01.2 -> 02:00.2 (AMD chipset switch up) -> 03:00.0 (down) -> 04:00.0 ->
05:01.0 -> 06:00.0`, where `04:00.0`/`05:01.0` are Intel switch parts
(`8086:4fa0` / `8086:4fa4`) that `lspci` cannot name. The B60 sits behind its own
Intel bridge pair (`8086:e2ff` / `8086:e2f0`) on the Gen4 x8 port.

**The endpoint link registers are unreliable on this topology and must not be
used.** Both GPUs and their immediate upstream ports report
`max_link_speed=2.5 GT/s, max_link_width=1` — PCIe 1.0 x1, ~250 MB/s — in both
sysfs and `lspci` config space, with the A770 in D0. That is an unpopulated field
on those switch parts, not the link: `04:00.0` advertises
`Capabilities: [188] Physical Layer 16.0 GT/s`, and this repository's own batched
NVMe->VRAM cell moved 1 MiB in ~1148 us (~0.9 GB/s), already 3.6x above what a
Gen1 x1 link could carry. **Read the CPU root ports, not the endpoints.**

Consequences that bear on any streaming design:

- NVMe and the A770 are on **separate root ports**, so peer-to-peer traffic goes
  up `00:01.1`, across the root complex and back down `00:01.2`. This is why
  `pci_p2pdma_distance()` reports 8 for the A770 and the mapping type is
  `THRU_HOST_BRIDGE`. The B60's 6 is the shorter Gen4 x8 path.
- **The A770's host-to-device ceiling is ~3.9 GB/s**, on the same order as the
  NVMe's own ~2.3 GB/s raw. The B60's is ~15.75 GB/s, roughly 4x.
- Any projection that feeds VRAM at DRAM read bandwidth (44.4 GiB/s) is off by
  ~12x for the A770 on this machine, because the DRAM still has to cross a Gen3 x4
  link to reach the card.

**What this does NOT settle:** a link width is a ceiling, not an achieved rate.
Whether a host-fed path actually reaches its ceiling — and whether a direct
NVMe->VRAM path reaches the drive's — is unmeasured in both directions. That
measurement is the gate of `docs/campaigns/miss-tier-direct.md`, and this table
is why it must be taken per card.

## The target box is AMD, not Intel

`/sys/class/iommu/` is `ivhd0` (AMD-Vi). `dmesg`: `iommu: Default domain type:
Translated` — the IOMMU **translates**; there is no `iommu=pt` or `amd_iommu=off`
on the cmdline. Any design that puts a raw physical address where a bus address
belongs is therefore wrong by construction.

Topology (`lspci -t`): NVMe `0000:01:00.0` under root port `00:01.1`; A770
`0000:06:00.0` under `05:01.0`; B60 (Battlemage G21, `[8086:e211]`)
`0000:0f:00.0` under `0e:01.0`. Same root complex, different root ports, so P2P
traverses the host bridge — `pci_p2pdma_distance() = 8`, i.e.
`PCI_P2PDMA_MAP_THRU_HOST_BRIDGE`, whose documented mapping is a normal
`dma_map_*` IOVA.

**NVMe and the A770 are in IOMMU group 0; the B60 is in group 2.** That is an
independent reason the A770 is the correct first target.

`nvme.ko` imports `blk_rq_dma_map_iter_start` / `blk_rq_dma_map_iter_next`
(verified with `nm -u`), so this kernel's NVMe driver handles P2P natively.
`struct bio_vec` still holds `struct page *bv_page`, so a bio still needs pages —
which is what the p2pdma carve provides.

## Ops: swapping xe on the target

Both GPUs share one `xe.ko` and both serve arcint. Stopping the arcint units is
mandatory before `rmmod`; `rmmod` also needs both GPUs unbound (driver-bind and
mei late-bind refs). `insmod` skips the boot cmdline, so an A770 needs
`modprobe xe force_probe=56a0`. A failed probe leaves stale resource claims
("Resources present before probing") — fix with PCI `remove` + `rescan`, but
that destroys the container's `/dev/dri` bind mounts, so recreate the render
nodes afterwards. Rebuilds of `xe.ko` need the `xe_gen_wa_oob` prebuild run
manually; `M=modules` does not run it.
