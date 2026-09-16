# arcwell

A Linux kernel module that makes an NVMe controller DMA mixture-of-experts weights
**straight into Intel Arc VRAM**. No host RAM staging buffer, no page cache, no
per-read filesystem walk. The bytes go controller → GPU BAR and the CPU never
touches them.

Version 0.0.1. Built and measured on an Intel Arc Pro B60. Not yet consumed by an
inference engine — that is the open work, and this README exists to make adopting
it a decision made on evidence rather than on the pitch.

---

## The one fact that decides your design

**arcwell is a throughput mechanism, not a latency one.**

| | arcwell | host-fed path |
|---|---|---|
| Bandwidth, cold | **2.91 GB/s** | 1.58 GB/s |
| CPU per GiB | **0.0121 s** | 0.1971 s |
| Latency, one expert | **1.125 ms** | — |

That last row is the trap. Fetching one expert takes 1.125 ms; the host CPU can
*compute* that same expert in 198 µs. **Called synchronously on the decode path,
arcwell loses to doing nothing at all.** It only wins when the fetch is issued
ahead of need and overlapped with compute, which is what `AW_IOC_SUBMIT_BATCH` and
`AW_IOC_BATCH_WAIT` exist for.

This is not a hypothetical failure mode. It is the same defect arcint already hit
once with M9's synchronous slot upload: 0.4 t/s synchronous, 9.1 t/s once
overlapped with compute, same hardware. Re-issuing arcwell synchronously
reproduces that with a worse constant.

Read [`docs/USING_ARCWELL.md`](docs/USING_ARCWELL.md) before writing any client
code. It is short, and §4 is the part that matters.

## What is measured, and what is not

**Measured**, red-first, every number traceable to a file in [`results/`](results/):

- NVMe controller DMA lands real disk bytes in a GPU-owned VRAM BO, verified
  through the BO's own mapping — `results/BO_GATE`, `results/M0_GATE`
- An OpenCL kernel consumes those bytes with no host bounce, fenced on a
  `cl_event` — `results/E2E`, `results/CL_IMPORT`
- 2.91 GB/s against a cold host-fed 1.58 GB/s, at 16.3× less CPU per GiB —
  `results/B_FABRIC`, `results/LATENCY`
- Async submit returns in 18.3% of a batch's wall time; four batches overlap
  cleanly; closing the fd drains in-flight work — `results/ASYNC`
- Ranges DMA'd out of a real 64-extent GGUF shard, interior and
  boundary-spanning — `results/GGUF_OPTION_A`
- `via_host_bounce` refuses rather than degrades, proven by forcing it —
  `results/BOUNCE_DETECTOR`

**Not proven.** Stated here rather than left for you to discover:

- **That prefetch actually hides the 1.125 ms against a real routing trace.**
  Everything above shows only that hiding it is *possible*. This is the question
  integration answers, and it is the reason 0.0.1 makes no inference-gain claim.
- The host-fed comparand is an equivalent staging path, not arcint's own ring.
- Everything here is the Arc Pro B60. The A770's PCIe path goes unresponsive
  under load and that card is backlogged; no number in this repo is from it.

`M4_RESULTS.json` keeps `gain_demonstrated: false` deliberately. A transfer gain
is measured; an inference gain is not.

## The API

Seven ioctls on `/dev/arcwell`. The contract is
[`stub/include/aw_uapi.h`](stub/include/aw_uapi.h) — there is no client library
to link, and no CUDA dependency or naming.

| cuFile / GPUDirect | arcwell |
|---|---|
| `cuFileBufRegister` | `AW_IOC_MAP_BUFFER` |
| `cuFileBufDeregister` | `AW_IOC_UNMAP_BUFFER` |
| `cuFileRead` | `AW_IOC_READ_BLOCKS` |
| — | `AW_IOC_READ_BATCH` (synchronous batch) |
| `cuFileBatchIOSubmit` | `AW_IOC_SUBMIT_BATCH` |
| `cuFileBatchIOGetStatus` | `AW_IOC_BATCH_WAIT` |
| — | `AW_IOC_STATS` (so a host bounce cannot hide) |

The device-pointer analog is a **buffer handle**, not a pointer. There is no file
handle: arcwell takes absolute NVMe LBAs, and file→LBA translation is the caller's
once-at-open job (`stub/tools/aw_fiemap.c` is the reference implementation).

Buffers are owned by the file descriptor. Closing it releases every registration
and drains every in-flight batch, so a crashing client cannot strand VRAM.

## What an integrator has to provide

Satisfy these or arcwell will refuse the mapping — which is the intended
behaviour, not an obstacle:

| | |
|---|---|
| Buffer | a dma-buf fd exported from an xe VRAM BO: `NEEDS_VISIBLE_VRAM`, `CPU_CACHING_WC`, size a multiple of 64 KiB |
| Alignment | `in_dest_offset` and transfer length page-aligned |
| Storage | a filesystem that implements FIEMAP. **ZFS and btrfs do not.** ext4 does |
| Layout | one file per expert, `fallocate`d, gives exactly one extent per expert, so an expert is **one request**. Ranges inside a big GGUF also work, but some will span an extent boundary and become two. It does **not** make an expert one bio — `BIO_MAX_VECS` caps a bio at 1 MiB, so larger experts always split |
| Topology | GPU and NVMe reachable for P2P (`pci_p2pdma_distance() >= 0`) |
| Batch | 1..256 requests |

**Two checks that are not optional in a client:**

1. Assert `out_flags & AW_MAP_F_REQUIRE_P2P` after every `AW_IOC_MAP_BUFFER`. xe
   *silently* disables peer-to-peer when `pci_p2pdma_distance() < 0` and migrates
   the buffer to system RAM. A client that does not re-check gets a
   working-looking path that quietly runs through host memory — the exact thing
   this project exists to remove.
2. Read `AW_IOC_STATS.via_host_bounce` **as a delta around your own work**. The
   counter is module-global, so an absolute reading includes other clients'
   refusals. Nonzero means a caller asked for something misconfigured; no code
   path bounces — both sites return an error.

## Install

```sh
sudo ./packaging/dkms/install.sh          # DKMS, udev rule, VRAM detector
sudo arcwell-detect-vram <gpu-bdf> <nvme-bdf> <block-device>
```

The detector exists because `vram_usable` cannot be guessed: `pci_resource_len()`
returns the BAR aperture, not memory. On one card here that is a 32 GiB BAR over
24 GiB of VRAM, and carving by BAR length puts DMA-able pages over memory that is
not there. The figure comes from xe's `"usable size exclude stolen"` probe line.

To build without installing:

```sh
make -C /lib/modules/$(uname -r)/build M=$PWD/stub/src modules
```

That builds `arcwell.ko` plus three probe modules that are the evidence for the
rules in `KERNEL_FACTS.md`; they are not part of the data path.

## Repository map

| path | what it is |
|---|---|
| `stub/src/arcwell.c` | **the module** — the whole product, ~1130 lines |
| `stub/include/aw_uapi.h` | the contract. The only interface that is promised |
| `docs/USING_ARCWELL.md` | how to drive it and the two ways to get it wrong |
| `results/` | every measurement, raw, with the command that produced it |
| `KERNEL_FACTS.md` | kernel-level rules established by probe, with retractions kept visible |
| `BACKLOG.md` | the full campaign record, including every defect and disproof |
| `stub/test/` | acceptance cells, each red-first |
| `packaging/dkms/` | DKMS, udev rule, VRAM detector |
| `REVIEWER_PROMPT.md` | the standing review brief; §12 covers the newest material |

## On reading this repository

`BACKLOG.md` and `KERNEL_FACTS.md` keep superseded and retracted claims visible
rather than editing them away, because several of this project's hardest blockers
turned out to be self-inflicted and the record of how is worth more than a clean
narrative. Two examples, both load-bearing for anyone evaluating the approach:

- **"Blocker B1: p2pdma deadlocks on the Arc BAR"** was an ABI bug in a locally
  declared extern — the real `pci_p2pdma_add_resource()` takes four arguments, not
  five. It returns 0 in 9 ms.
- **"A 128 MiB carve destroys write-combining"** was never tested until it was, and
  it is false. CPU reads of VRAM through a WC mmap cost ~1.4 µs per access whether
  or not the range is carved. `results/WC_CARVE`.

If a claim in this repo is not backed by a file in `results/`, treat it as
unproven. That rule is applied to its own claims above.
