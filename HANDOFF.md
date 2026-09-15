# arcwell HANDOFF

Card: arcwell — a kernel module that streams mixture-of-experts expert weights
from NVMe into Intel Arc GPU VRAM by controller DMA. First user: arcint.

Read `KERNEL_FACTS.md` before anything else. It is the single authoritative
source for what the hardware and kernel do. This file carries only governance
state and what is left to do.

## Standing architecture (not open for reconsideration)

Goal of this project is DMA from NVME to GPU VRAM of Intel Arc A770 and Intel Arc B60 for efficient LLM weight streaming. No host RAM Buffer - except for housekeeping of necessary metadata - is allowed

arcwell is a **kernel module**. The delivery path is the module's ioctl surface:
`stub/include/aw_uapi.h` — `AW_IOC_MAP_BUFFER` (register a VRAM buffer as an
NVMe DMA target), `AW_IOC_READ_BLOCKS` (issue storage→VRAM DMA over a raw block
range), `AW_IOC_STATS` (so a bounce cannot hide).

There is no userspace data path, and none is to be designed, benchmarked, or
proposed. A userspace read lands block-device bytes in host pages before any
userspace code can move them, so any userspace "direct storage" path *is* the
host bounce buffer — the thing this project exists to remove. Decided at leg 006
after direct measurement.

The only userspace-shaped thing allowed in the tree is a thin ioctl client that
issues the calls above. It holds no weight bytes and is not a data path.

## Governance

See `HOUSE_RULES.md`. Seats: Operator (human, owns remote and merges), Engineer
(builds and commits on `dev`, never pushes master), Reviewer (classifies from a
fresh archive, never edits code), Fix (implements reviewer-named fixes). Work
lands on `dev` marked `BATCH-PENDING-REVIEW`. Red-first cells only — a cell that
cannot fail is decoration. Close every leg with a cycle account. No hostnames,
addresses, or credentials in tracked files.

## Where the work stands

| Item | State at 0.0.1 |
|---|---|
| Target buffer (host-visible VRAM BO) | **works on stock xe** — recipe in `KERNEL_FACTS.md` |
| NVMe-side DMA mapping of BAR memory | **solved** — `dma_map_resource()` returns a valid IOMMU-translated address; the old "B2" was never actually tested |
| `p2pdma` carve of Arc BAR2 | **works** — the old "B1 deadlock" was an ABI bug in a locally-declared extern, not a kernel limit. Returns 0 in 9 ms |
| Module ioctl surface implemented | **yes** — seven ioctls, `stub/src/arcwell.c` |
| Bytes ever landed in Arc VRAM by a controller DMA | **yes** — red-first, `results/M0_GATE`, `results/E2E` |
| Consumed by an OpenCL kernel with no host bounce | **yes** — `results/E2E`, fenced on a `cl_event` |
| Performance gain vs the incumbent | **measured, not claimed**: 2.91 GB/s cold vs a host-fed 1.58, at 16.3x less CPU per GiB. Per-expert latency 1.125 ms — see `docs/USING_ARCWELL.md` §1 before reading that as a win |
| Integrated with an inference engine | **no** — the open work |

Module sources live in `stub/src/`. `stub/src/arcwell.c` **is** the product: a
misc char device serving seven ioctls (`stub/include/aw_uapi.h`). It carves the
Arc BAR with `pci_p2pdma_add_resource()`, imports a host-visible VRAM BO's
dma-buf, recovers the BAR physical behind the IOMMU translation, builds
`struct page`s over it, and submits bios straight at the block device. Alongside
it build three probes (`aw_dma_map_probe`, `aw_p2p_provider_probe`,
`aw_m0_gate`) which are the evidence for the `KERNEL_FACTS.md` rules, not part
of the data path.

Acceptance cells live in `stub/test/`, each red-first. `tools/common/`
(`fiemap_probe.py`, `consumption_probe.py`) and `stub/tools/aw_fiemap.c` handle
file→LBA translation.

## Next step

The engine is built and measured; the open work is **integration**. arcwell is a
throughput mechanism, not a latency one — 1.125 ms per expert against 198 µs of
CPU compute means a consumer that calls it synchronously on the critical path
will be slower than not using it at all. The async surface
(`AW_IOC_SUBMIT_BATCH` / `AW_IOC_BATCH_WAIT`) exists so a consumer can prefetch
the next layer's experts behind the current layer's compute. Whether that
actually hides the latency in arcint is **unproven** and is the next thing to
measure. Read `docs/USING_ARCWELL.md` first.

Standing contract for any integration: `via_host_bounce` in `AW_IOC_STATS` must
stay 0 across the run. If it is nonzero the build did not satisfy the card and
must say so rather than pass.

## Cycle account (current leg)

- moved — the tree can no longer route an agent onto a userspace data path: 39
  historical records and their dependent cells removed (leg-by-leg milestone
  docs, results JSON, refuted userspace probes, the userspace research notes,
  and the session handover that restated the retired v1 boundary); the durable
  measured facts they contained were distilled into `KERNEL_FACTS.md` before
  deletion; `M4_RESULTS.json` and its two cells rewritten to state the decision
  without citing deleted measurements.
- moved — surviving records now speak in ioctl terms (`AW_IOC_MAP_BUFFER` /
  `AW_IOC_READ_BLOCKS`) instead of the retired client-library names; suite 2
  passed.
- exception — leg history is no longer in the tree; cost: the reasoning behind
  individual past decisions lives only in git history, so a reviewer wanting it
  must read `git log`; bought: an agent reading this tree sees one architecture,
  one set of measured kernel facts, and the one open gate.
