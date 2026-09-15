# arcwell against the arcint record — RTFM disposition, 2026-09-15

Written after reading the arcint working tree per its own RTFM mandate
(`CLAUDE.md`, `docs/campaigns/README.md`, `docs/sop-card-window.md`,
`docs/design-qwen-flash-next.md`, `docs/milestone-0.3.0.md`,
`src/exec/backend_ov.cpp`). Evidence class is stated per row, as the mandate
requires. `record` means measured by arcint and read here, not re-measured by me.

---

# 0. SETTLED — do not reopen

`docs/design-qwen-flash-next.md:1084`: *"The fully-resident plan does not fit
(WP6); the **streaming plan fits one A770 at ~18 t/s as-shipped and ~30-40 t/s
with an MTP head re-exported**, with a fast (NVMe) miss tier as the
prerequisite."*

**"The model does not fit in RAM" is a closed question.** It is the premise of the
streaming design, not a finding to be rediscovered. The arcint RTFM mandate exists
in part to stop exactly this debate being had again; I reopened it once in this
document and struck it. Streaming is the plan, the NVMe miss tier is its named
prerequisite, and **arcwell is the component that makes that miss tier direct.**
Work from there.

What is genuinely open is narrow, and the record says so itself: the miss tier's
rate has never been measured on this hardware from either direction. That, and
only that, is §6.

## 1. The incumbent arcwell was told to beat no longer exists

`ENGINEER_PROMPT.md` §6: "Measure against arcint's incumbent slot-upload path
(its recorded figure is 309 us per tensor)."

**[record]** `docs/milestone-0.3.0.md`, M9 row, 2026-09-02: *"The measured blocker
falls: slot-upload time overlapped with compute instead of 309 µs/tensor
synchronous"* and *"Counters read after the flush fix: avg copy 309→5 µs/tensor"*.
Async batched uploads with a device-tier pool took it from 0.4 to 9.1 t/s.

**[record]** `docs/design-m9-offload-v2.md:61` further flags the 309 figure as
possibly misattributed: 13.1 s ÷ 309 µs ≈ 42.4k tensor loads against ≈12k implied
by the routing arithmetic — *"if they do not reconcile, 309 µs is per something
other than a tensor"*.

**[record]** `docs/milestone-0.3.0.md`, M14: computing offloaded expert FFNs on
the host beats uploading them at all — tier ON 15.0/15.5 t/s vs OFF 10.4/10.6.

**CORRECTION, 2026-09-15.** I first read M14 as a competitor to arcwell. That was
wrong. FreeToken's `q* ≈ m · B_P / B_H` routes work to the CPU when `B_P` is low,
so the CPU tier and a direct DMA path are complementary: arcwell takes the CPU out
of the copy path, and the CPU tier is what consumes the headroom that frees. On
the A770, whose `B_FABRIC` measures 1.84 GB/s, this is the dominant component
rather than a secondary one. See `docs/campaigns/miss-tier-direct.md`,
"Bandwidth is not the only currency".

**Disposition:** the 309 µs bar in `ENGINEER_PROMPT.md` is stale by two weeks and
was contested when written. Any arcwell benchmark cell citing it measures nothing.

## 2. What the record says arcwell's data path is actually worth

**[record]** `docs/design-qwen-flash-next.md:1021-1024`, measured tier bandwidths:

| tier | measured |
|---|---|
| DRAM read (the DRAM->VRAM feed) | ~44.4 GiB/s |
| NVMe sequential read, **raw** | ~2.3 GB/s |
| NVMe **in-container through ZFS** ("the real miss-feed rate") | ~1.8 GB/s = 1.68 GiB/s |
| HDD-backed store, cold | ~0.41 GB/s |

**[record]** Same doc: *"Miss tier is decisive. The NVMe (measured ~1.68 GiB/s
in-container) gives ~18 t/s at 95% hit; the HDD tier (0.41 GB/s) gives ~6 t/s — a
~3x swing."* And *"+8 GiB resident ~= +5-6 t/s in the 16->40 GiB range."*

**Disposition — arcwell's value proposition, restated in the record's own terms.**
Two components, neither of which is "replace a slow copy":

1. **Miss-tier bandwidth.** Today's feed is 1.68 GiB/s *through ZFS in a
   container*; the raw device measures 2.3 GB/s. A direct NVMe->VRAM path is
   bounded above by the raw figure, so the ceiling on this component is roughly
   1.28x, not an order of magnitude.
2. **DRAM returned to the expert LRU.** Removing host staging returns DRAM to
   residency, which the record prices directly at +5-6 t/s per +8 GiB.

## 3. A cheaper experiment must run before arcwell is credited with (1)

**[record]** the 1.68 GiB/s figure is explicitly *"in-container through ZFS"*,
against 2.3 GB/s raw on the same device. Most of that 1.37x gap is filesystem and
container overhead, not the absence of peer-to-peer DMA.

The operator has already decided (2026-09-15) to create an **ext4** for expert
storage, because neither ZFS nor btrfs provides FIEMAP. **That change alone may
recover most of component (1) without arcwell.** Measuring the miss-feed rate on
ext4 before building the integration is the honest ordering; otherwise arcwell
gets credited for a gain a filesystem change delivered.

## 4. Integration mechanics — resolved, and it is an arcint-side change

**[code]** `src/exec/backend_ov.cpp` allocates device memory through
`ov::RemoteTensor` from the OpenVINO remote context (`rctx.create_tensor(...)`,
lines 5746/5782/5808). It uses `ov::intel_gpu::shared_mem_type` with
`SharedMemType::USM_HOST_BUFFER` (6206, 8025). **There is no dma-buf path
anywhere in it** — a grep for `dma_buf|prime|export_handle` returns only false
positives on "primed".

**[code]** `<openvino include dir>/openvino/runtime/intel_gpu/ocl/ocl.hpp`:
`ClBufferTensor : public RemoteTensor` exposes `cl_mem get()` and is obtained
from `ClContext::create_tensor()` with `shared_mem_type` + `mem_handle` params —
i.e. OpenVINO can **wrap an existing `cl_mem`**.

**[measured-here]** this session: an xe VRAM BO exported as a dma-buf imports into
OpenCL via `cl_khr_external_memory_dma_buf` (handle type 0x2067, both cards) and a
GPU kernel reads it; `results/CL_IMPORT_2026-09-15.txt`.

**Disposition:** the viable chain is
`xe GEM BO -> dma-buf fd -> cl_mem (external memory import) -> ClBufferTensor ->
OpenVINO`. arcwell owns the allocation and hands OpenVINO a tensor over it.
That is a change to how arcint allocates, not a call swap at the upload site.

## 5. Where arcwell sits in arcint's actual plan

**[record]** `docs/campaigns/README.md` lists fourteen campaigns. **None is
NVMe->VRAM streaming.** The two expert-weight campaigns are
`sub4bit-vram-kernel` (shrink the weights so more stay resident) and
`kquant-host-storage` (compute offloaded experts on the host in K-quant form).
Both reduce or eliminate bus traffic rather than accelerate it.

**Disposition:** arcwell is not currently a campaign in the arcint record. Under
`docs/campaigns/README.md`'s rules it would need one before integration work
starts: a charter, a gate that can fail, entry criteria, and the measurement that
defines the defect. The gate cannot be the 309 µs figure (§1).

---

## What this does NOT change

The kernel result stands on its own and is unaffected by any of the above:
NVMe controller DMA lands disk bytes in a GPU-owned Arc VRAM BO with
`via_host_bounce=0`, verified red-first on both the A770 and the Arc Pro B60, and
consumed by an OpenCL kernel fenced on a `cl_event`
(`results/E2E_2026-09-15.txt`, `results/B60_PARITY_2026-09-15.txt`).
`KERNEL_FACTS.md` records that no source-verified Intel Arc NVMe->VRAM P2P
demonstration existed; there is one now. What the record changes is what that
capability is *worth*, and against what it should be measured.

---

# 6. The premise, stated so it can fail

Operator, 2026-09-15: *"arcwell is the component that will make arcint outperform
FreeToken if the premise holds."* Below is the premise made falsifiable, and the
one number the whole thing turns on.

## FreeToken's mechanism is a bandwidth race, and `B_P` is one of its two terms

**[record]** `docs/research-freetoken.md:19-36`: FreeToken splits the missing
experts of a decode step between streaming over PCIe (rate `B_P`) and executing
in place on the CPU (equivalent rate `B_H`), balancing at `q* ≈ m · B_P / B_H`
(their Equation 4). The routing decision *is* the ratio of those two rates.

**[record]** `docs/research-freetoken.md:169`: FreeToken's published row is an
**RTX 5090, PCIe 5.0 x16, B_P 49.0 GB/s**. `:40` records the deviation
explicitly — *"arcint's `B_P` is over PCIe 4.0 x16 to an Intel..."* — i.e. about
half the lane bandwidth of the published row, on a different vendor's stack.

## `B_P` HAS NEVER BEEN MEASURED ON THIS HARDWARE

**[record]** `docs/design-qwen-flash-next.md:1315-1320` specifies exactly how to
get it and warns against the shortcut: *"Measured, not read off a spec sheet: the
design doc's own FIX C section above already treats PCIe 4.0 x16's '~28 GB/s' as a
nameplate figure to be checked against, not trusted."* `:1494` lists *"measured
`B_P` (PCIe stream rate) on the card in scope"* as an unmet requirement.

**[record]** The one place a PCIe rate is used, it is a hypothesis that was then
falsified. M14's envelope: *"2.5 MB int4 expert ≈ 50 µs from DDR4 vs ≈ 175 µs over
PCIe"* — labelled *"Envelope (hypothesis, to be measured)"* — and the measured
outcome lists among its three defects *"the envelope's bandwidth assumption"*.
175 µs for 2.5 MB implies ~14.3 GB/s, and the record says that assumption did not
survive contact.

**So the 40.4 t/s "FreeToken regime" ceiling is not a bus measurement.** It is
`1.0986 GiB/token` divided by **44.4 GiB/s**, and `:1875` shows that 44.4 GiB/s is
*DRAM read bandwidth on the Zen 3 SoC, single-threaded* — a memory-subsystem
number. A host-fed stream must still cross PCIe 4.0 x16 to reach VRAM, and that
leg is unpriced anywhere in the record.

## The premise, falsifiable

> For an expert pool that exceeds host DRAM, serving rate is bounded by the
> miss-feed path. arcwell replaces `NVMe -> ARC/page cache -> host buffer ->
> PCIe -> VRAM` with `NVMe -> PCIe -> VRAM`, removing DRAM bandwidth, the CPU and
> the staging copy from that path. The premise holds iff the host-fed path's real
> `B_P` is materially below what the PCIe fabric can deliver, AND arcwell's direct
> path can approach the fabric limit.

**Where it fails:** a single NVMe measures ~2.3 GB/s raw
(`design-qwen-flash-next.md:1022`). If the host-fed `B_P` is well above that, one
drive cannot win on bandwidth and arcwell's value collapses back to freed DRAM
(+5-6 t/s per +8 GiB, `:1071`). Winning on bandwidth beyond one drive is a
hardware decision, not a code one.

## The gate this campaign should carry

`docs/campaigns/README.md` requires a gate on the record before work starts, and
one that can fail. Proposed, two numbers on the same card, same expert-slot size,
same batching:

1. **`B_P_host`** — the existing staging-ring path, measured exactly as
   `design-qwen-flash-next.md:1315` specifies (repeated non-blocking upload of an
   `expert_slot_bytes`-sized buffer, amortised, batched not per-tensor).
2. **`B_P_arcwell`** — the identical transfer size and batching, NVMe -> VRAM
   through `AW_IOC_READ_BATCH`.

`B_P_arcwell < B_P_host` kills the bandwidth premise and the campaign closes as a
verdict — which `campaigns/README.md` explicitly permits — with arcwell's value
restated as DRAM and capacity only. Neither number exists today; **`B_P_host` is
the more important of the two, it is arcint's own unmet requirement, and it does
not need arcwell at all to obtain.**

## What this does not license

No arcwell figure measured so far is a `B_P`. Every `us=` in `results/` came from
an unoptimised single-shot or 16-deep path with no amortisation, on a cell built
to prove correctness, not rate. Quoting any of them against 44.4 GiB/s, 49.0 GB/s
or 309 µs would be exactly the error this document exists to prevent.
