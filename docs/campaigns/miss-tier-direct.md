# miss-tier-direct — the expert miss tier feeds VRAM from NVMe without transiting host DRAM

## The defect, as measured

`docs/design-qwen-flash-next.md:1054` (arcint): *"Miss tier is decisive. The NVMe
(measured ~1.68 GiB/s in-container) gives ~18 t/s at 95% hit; the HDD tier
(0.41 GB/s) gives ~6 t/s — a ~3x swing."* The streaming plan is the plan
(`:1084`); the miss tier is its named prerequisite.

Today that tier runs `NVMe -> ZFS/ARC -> host buffer -> PCIe -> VRAM`. Three
things about it are on the record and none of them is a bus measurement:

- `:1022` NVMe measures **~2.3 GB/s raw** but **1.68 GiB/s in-container through
  ZFS** — a 1.37x gap that is filesystem and container overhead. [record]
- `:1875` the 44.4 GiB/s used to derive the 40.4 t/s "FreeToken regime" ceiling is
  **DRAM read bandwidth on the Zen 3 SoC, single-threaded** — a memory-subsystem
  number, not a host-to-device transfer. [record]
- `:1315`, `:1494` **`B_P`, the PCIe stream rate, has never been measured on this
  hardware**, and the doc warns against substituting the nameplate: *"PCIe 4.0
  x16's '~28 GB/s' as a nameplate figure to be checked against, not trusted."*
  The only PCIe figure anywhere is M14's envelope hypothesis (~175 µs for a 2.5 MB
  expert ~= 14.3 GB/s), and *"the envelope's bandwidth assumption"* is listed among
  that milestone's three measured defects. [record]

So the miss tier's rate is unmeasured from **both** directions, and the ceiling
everyone reasons from is a DRAM number for a path that must cross PCIe.

## Known against hypothesised

**Known [measured-here, this repository, 2026-09-15]:** an NVMe controller DMAs
disk bytes directly into a GPU-owned Arc VRAM BO with `via_host_bounce=0`,
red-first on the A770 and the Arc Pro B60, consumed by an OpenCL kernel fenced on
a `cl_event`. The uAPI serves map/read/batch/unmap/stats.
(`results/E2E_2026-09-15.txt`, `results/B60_PARITY_2026-09-15.txt`.)

**Hypothesised, and the whole point of this campaign:** that a direct feed beats
the host-fed one at the rate that matters. Nothing measured so far bears on this.
Every `us=` figure in `results/` came from a correctness cell with no
amortisation and is NOT a `B_P`.

## Gate

Four numbers, **on the B60**, same `expert_slot_bytes`-sized transfer, same
batching, same window:

| symbol | what | how |
|---|---|---|
| `B_FABRIC` | what PCIe can deliver to this card at all | batched pinned host->device writes, amortised |
| `B_P_host_warm` | incumbent feed, bytes in ARC | staging-ring path per `design-qwen-flash-next.md:1315` |
| `B_P_host_cold` | **incumbent feed on a real miss** | same, ARC evicted, state recorded |
| `B_P_arcwell` | the direct feed | identical size and batching via `AW_IOC_READ_BATCH` |

**The gate can fail, and these are the failures:**

- `B_P_arcwell < B_P_host_cold` -> the bandwidth premise is dead. The campaign closes
  as a **verdict**, arcwell's value restated as freed DRAM only (`:1071` prices it
  at +5-6 t/s per +8 GiB), and no integration is built.
- `B_P_host_cold` already close to `B_FABRIC` -> no headroom, same verdict.
- `B_P_arcwell` capped near 2.3 GB/s while `B_FABRIC` is much higher -> a single
  drive cannot saturate the fabric. Not a code outcome: it is a hardware question
  (aggregate drives), recorded as such and not pursued in this campaign.

## The comparison must be a MISS, or it measures the wrong thing

Scoped to the **Arc Pro B60 (`0f:00.0`, Gen4 x8, ~15.75 GB/s)**. The A770's
chipset-attached Gen3 x4 (~3.9 GB/s) sits at the NVMe's own rate, so it cannot
separate the two paths whatever it measures. The board has the lanes it has; this
is a scoping fact, not a defect.

On the B60 a naive reading says the host-fed path wins ~6x: 15.75 GB/s of link
against one drive's ~2.3 GB/s. **That reading is wrong, because it prices a DRAM
hit, not a miss.** The miss tier is by definition the path taken when the bytes
are NOT in DRAM. For a genuine miss both paths start at the same drive:

| | host-fed miss | arcwell miss |
|---|---|---|
| | NVMe -> ARC/page cache -> host buffer -> PCIe -> VRAM | NVMe -> PCIe -> VRAM |
| bounded by | the drive, plus a DRAM round-trip and CPU | the drive |

So the B60's 15.75 GB/s link only matters for bytes already resident in DRAM,
which is residency, not streaming. The premise is not "beat the link"; it is
"remove the DRAM round-trip from a path that is drive-bound at both ends".

**Therefore `B_P_host` must be measured COLD** — ARC evicted / page cache dropped,
the file not previously read in this boot — and the state recorded, because ZFS
file data lives in the ARC and is *not* visible in `MemAvailable` or `fincore`
(arcint retracted a "page cache cold" reading for exactly this reason,
`docs/milestone-0.3.0.md` §7.0.2af). A warm `B_P_host` measures DRAM->VRAM, which is
fast, so comparing arcwell against it would hand arcwell a false **loss** — the
gate would declare the premise dead by pricing a hit against a miss. A cold one
is the miss tier and is the honest comparand.

Both a cold and a warm `B_P_host` are worth having: warm bounds what residency
buys, cold is the number this gate turns on.

## Bandwidth is not the only currency: the CPU is the other one

Operator, 2026-09-15: *"A770 still wins with the FreeToken approach because the
CPU will have time for other stuff needed in this high bit copy environment."*

This corrects a framing error in `RTFM_DISPOSITION.md` §1, which read M14's CPU
tier as a **competitor** to arcwell ("arcint measured that not uploading is
faster"). It is not. They are complementary, and on the A770 they are the same
argument:

- **[record]** FreeToken splits a decode step's missing experts between PCIe
  streaming (`B_P`) and in-place CPU execution (`B_H`), balancing at
  `q* ≈ m · B_P / B_H` (`docs/research-freetoken.md:33`). A **low** `B_P` pushes
  work onto the CPU, it does not stall the step.
- **[measured-here]** `B_FABRIC(A770) = 1.84 GB/s`, below the drive's own supply.
  So on that card the split is CPU-heavy by construction, and CPU headroom — not
  bus bandwidth — is what decides the rate.
- **[record]** M14 already measured the host kernel at **198 µs/expert from RAM**
  and the tier winning 15.0/15.5 t/s against 10.4/10.6
  (`docs/milestone-0.3.0.md`). That tier needs a CPU that is free.
- The host-fed miss path spends CPU on precisely what arcwell does with **none**:
  read syscalls, ARC/page-cache traversal, and a staging copy into pinned memory.
  Every CPU-second the copy path does not take is a CPU-second the `B_H` tier can
  use.

**So arcwell's value has a third component, and on the A770 it is the dominant
one:** it returns the CPU to the compute tier. That is measurable and nobody has
measured it.

### Added to the gate

| symbol | what | how |
|---|---|---|
| `C_host` | CPU-seconds per GiB moved, host-fed | `/proc/<pid>/stat` utime+stime plus system-wide busy delta over a sustained transfer |
| `C_arcwell` | CPU-seconds per GiB moved, direct | same accounting over `AW_IOC_READ_BATCH` |

`C_arcwell >= C_host` would kill this component outright and is the red case.
Unlike the bandwidth terms, this one can be measured on the **A770** as well as
the B60 — the card that is ineligible on bandwidth is exactly the card where this
component matters most.

## Entry criteria

- `B_FABRIC` measured first. It bounds the other two and needs neither arcint nor
  arcwell.
- The ext4 expert store exists (operator decision 2026-09-15), because `B_P_host`
  measured on ZFS would credit arcwell with a filesystem change. Both `B_P_host`
  and `B_P_arcwell` are taken against the same filesystem.
- `docs/sop-card-window.md` (arcint) satisfied in full. Not optional: this
  repository ran a whole campaign without it and reproduced several of its dated
  incidents.

## Scope — in / out

**In:** the three measurements; the verdict; a campaign record.
**Out:** integration into arcint's allocation path; `out_segments` reporting real
DMA segments; `AW_READ_F_REVALIDATE`/`-ESTALE`; carve release. All wait on the
gate.

## Where it lives

`stub/src/arcwell.c` (module), `stub/include/aw_uapi.h` (uAPI),
`stub/test/aw_batch_test.c` (batched path), `results/` (evidence).
`B_P_host` needs arcint's staging ring (`patches/0006`).

## Invariants

`via_host_bounce` stays 0 or the run is void. Cells are red-first. Every number
names the card, the transfer size, the batching, the filesystem and the binary.
No figure is compared against 44.4 GiB/s, 49.0 GB/s or 309 µs — §1 and §6 of
`RTFM_DISPOSITION.md` say why.

## Status

- **2026-09-15 — opened.** Prior art read (`RTFM_DISPOSITION.md`). Gate above is
  on the record before any measurement, per `docs/campaigns/README.md`. Next
  action: `B_FABRIC`, under a sampler, SOP checklist first.
- **2026-09-15 — entry criteria partly met; the fabric is now on the record.**
  SOP §3 checklist run (cards free, load 0.07, ARC 4.59/40 GiB recorded, card
  identity by PCI id). PCIe topology measured and added to `KERNEL_FACTS.md`: the
  A770 is on a chipset-attached **Gen3 x4** root port (~3.9 GB/s), the B60 on
  **Gen4 x8** (~15.75 GB/s), the NVMe on its own Gen3 x4. Endpoint link registers
  read a bogus Gen1 x1 and must not be used.

  This makes `B_FABRIC` **per-card**, and it sharpens the gate's prediction: on the
  A770 the host-fed path draws from 44 GB/s DRAM but is squeezed through the same
  ~3.9 GB/s link that bounds the P2P path, while one NVMe supplies at most
  ~2.3 GB/s. On bandwidth alone the host-fed path should therefore win on the
  A770 unless it is inefficient enough to fall below the drive's rate — which is
  exactly the thing no one has measured. **Not a verdict: a link width is a
  ceiling, not an achieved rate.** Next action unchanged: measure `B_FABRIC`, then
  `B_P_host`, under a sampler.
- **2026-09-15 — `B_FABRIC` measured, entry criterion met.**
  `results/B_FABRIC_2026-09-15.txt`. **B60 = 13.99 GB/s** (89% of Gen4 x8);
  **A770 = 1.84 GB/s** (47% of Gen3 x4). Control ratios 14.6x / 5.0x, so neither
  run is void. Leg ran under its own sampler with a hard timeout and a clean
  zombie sweep.

  Two consequences. The A770's ineligibility is now a measured fact rather than an
  inference from link width: at 1.84 GB/s its ceiling is *below* the NVMe's own
  ~2.3 GB/s raw, so no miss-tier question can be separated on that card. And the
  B60 is **drive-bound, not fabric-bound** — 13.99 GB/s of bus against one drive's
  ~2.3 GB/s, i.e. a single NVMe can use ~16% of it. That is the regime the gate
  will decide in, and it is the regime where removing the DRAM round-trip is the
  whole of arcwell's bandwidth case.

  Next: `B_P_host_cold` on the B60, against the ext4 store, ARC state recorded.
- **2026-09-15 — the ext4 store exists; entry criteria met, with two cautions.**
  `/flash` on `nvme0n1p3`, ext4, 190.5 GiB, `noatime`, empty. Partition start
  sector **100665344** is the constant to add to FIEMAP's filesystem-relative
  `fe_physical`.

  **Caution 1 — contention.** The same NVMe carries rpool's SLOG (p1) and L2ARC
  (p2). Every sync write on rpool, from any guest, competes with expert reads on
  p3 over one controller and one Gen3 x4 link. `B_P_host_cold` and `B_P_arcwell`
  must be taken with the guests quiet, and the ARC/SLOG state recorded, or they
  measure contention. This extends SOP §3's "host quiet" to guest-quiet for
  storage legs.

  **Caution 2 — stale comparand.** `mkfs` zeroed the raw region earlier cells read
  at `0x1000000000`; it now sums to `0x00000000`, not `0x928a26d1`. Every cell
  taking `--expect` needs its comparand re-derived against the current disk before
  it is run again, or it fails for a reason that has nothing to do with arcwell.
- **2026-09-15 — ZFS contention measured, not assumed; suspension judged unnecessary.**
  Operator asked whether ZFS caching can be suspended for the window. Measured
  first: at idle with all seven guests up, `zpool iostat -v rpool` reports **0 ops
  and 0 bandwidth on both the SLOG and the L2ARC vdevs**; rpool's 415 KiB/s of
  writes all land on the raidz2 spinning disks. So the device contention is real
  in principle and currently zero, and suspending it would be configuration
  surgery on a 15.1 TiB production pool against a problem that is not occurring.

  It *can* be suspended if ever needed, least invasive first: monitor and void;
  `zfs set secondarycache=none` (stops L2ARC, instantly reversible);
  `zpool remove rpool <part1>` (drops the SLOG to the in-pool ZIL, supported live,
  re-addable); or stop the guests, which SOP §3 asks for anyway.

  The residual risk is not steady state but a **scheduled job starting mid-leg** —
  `timecapsule`, `rsync` and `nfs` are backup-shaped, and L2ARC is full (31.9 of
  32 GiB) so ARC churn could restart cache writes. A config change made before the
  leg would not catch that; detection during it does. The sampler now records
  per-interval NVMe read/write KiB/s plus SLOG and L2ARC sector deltas, and emits
  `CONTENTION: ... -- bandwidth number is VOID` on any nonzero log/cache traffic.
  Storage legs are void if that line appears.
- **2026-09-15 — first gate numbers taken, then invalidated by a defect they exposed.**
  `B_P_host_cold = 1.58 GB/s` (C_proc 0.2021, C_sys 0.3584 CPU-s/GiB) and
  `B_P_host_warm = 8.47 GB/s` (C_proc 0.1270, C_sys 0.1195) on the B60, zero
  contention lines. The 5.4x cold/warm gap confirms the campaign's insistence on
  a cold comparand: a warm number prices a DRAM hit, not a miss tier.

  `B_P_arcwell` did not run. At a realistic 2,457,600-byte expert (600 pages) the
  module hit `kernel BUG at block/bio.c:61` — `bio_alloc()` above `BIO_MAX_VECS`
  (256) BUGs rather than failing. Latent since the module's first version; every
  earlier cell used 1 MiB = exactly 256 pages and sat on the limit. Fixed by
  splitting in `aw_submit_request()`; recorded in `KERNEL_FACTS.md`.

  The two host numbers above were taken **before** the Oopses, on a clean kernel,
  but the box was tainted `[D]=DIE` afterwards. They are retained as indicative
  and **will be re-taken on the rebooted kernel** alongside `B_P_arcwell`, so that
  all four numbers of the gate come from one uncontaminated window.
- **2026-09-15 — GATE MEASURED. The premise holds on both currencies.**
  `results/GATE_2026-09-15.txt`. One clean window on a rebooted kernel, B60, zero
  contention lines on all three legs.

  | | B_P GB/s | C_proc | C_sys |
  |---|---:|---:|---:|
  | B_FABRIC | 13.99 | - | - |
  | **B_P_arcwell** | **2.91** | **0.0121** | **0.0341** |
  | **B_P_host_cold** | **1.58** | 0.1971 | 0.1963 |
  | B_P_host_warm | 8.51 | 0.1263 | 0.1280 |

  None of the three kill conditions fired: arcwell is **1.84x** the cold host path
  on bandwidth and costs **16.3x less process CPU per GiB**; the host path sits at
  11% of the fabric, so there was headroom to win in.

  The cold/warm split decided it. Against the warm path arcwell loses 2.91 to 8.51
  and the campaign would have closed as a verdict killing the premise — by pricing
  a DRAM hit against a miss.

  **Not closed.** Three things are owed before this is a disposition rather than a
  measurement: `B_P_host` against arcint's own staging ring rather than my
  equivalent; the CPU pair on the A770, where that component dominates; and a run
  against the real UD-Q3_K_XL GGUF, whose option-(A) layout can split an expert
  across extents. All four limitations are written into the result file.
- **2026-09-15 — the A770 CPU pair cannot be taken. That card wedges under a carve.**
  arcwell on the A770 measured `B_P=1.91 GB/s, C_proc=0.0390, C_sys=0.0512`,
  `via_host_bounce=0`, `max_inflight=193` — and then the **host** leg on the same
  card wedged it: `CT write: non-zero status: 4294967295`, repeating
  `ggtt_invalidate_gt_tlb` WARNs, two `aw_bp` processes unkillable by `SIGKILL`.
  Third wedge of the day on that card; the B60 ran the identical leg with 384 MiB
  carved and stayed healthy. Recorded in `KERNEL_FACTS.md`.

  So the campaign's "take the CPU pair on the A770, where it dominates" is
  **closed as not-possible on this hardware**, not deferred. That is a finding
  about the board, not about arcwell: the A770's own `B_FABRIC` (1.84 GB/s) is
  already below the drive, so nothing it could have measured would have separated
  the paths anyway.

  One number does survive from it, taken before the wedge and worth keeping: on
  the A770 arcwell reached **1.91 GB/s against a measured `B_FABRIC` of 1.84**.
  A P2P transfer slightly exceeding the host-to-device ceiling on the same card is
  consistent with the two paths not sharing a route (NVMe -> root -> GPU rather
  than DRAM -> root -> GPU), but it is within plausible run-to-run variance and is
  **not** claimed as a result.
- **2026-09-15 — carve clamped to usable VRAM; two defects found and fixed.**
  `pci_resource_len()` is the BAR aperture, not memory. On the A770 the BAR is
  16 GiB while usable VRAM ends 96 MiB lower; on the B60 the BAR is **32 GiB while
  VRAM is 24 GiB**. Carving by BAR length put ZONE_DEVICE pages over stolen or
  absent memory — measured: a 128 MiB carve at `0x65f8000000` ran to
  `0x6600000000`, past the usable end of `0x65fa000000`.

  Fixed with a `vram_usable` module parameter taken from xe's own
  `"usable size exclude stolen"` boot line, and a loud warning when it is unset.
  A second defect surfaced immediately: the granule walk-down broke on `-ERANGE`
  from the first granule, so a BO that fits a 2 MiB carve but not a 128 MiB one
  failed outright. `-ERANGE` (range outside usable, hopeless) is now distinct from
  `-ENOSPC` (this granule overruns, try smaller). Verified: the same allocation
  now takes a 32 MiB carve ending exactly at the limit, and the batch cell passes.

  **The gate numbers are unaffected.** The overrun registered pages beyond usable
  VRAM but never targeted them: DMA destinations are the BO's own pages, which xe
  allocates within usable memory. The extra coverage was dead registration, not
  stray writes.

  Also recorded as a measured negative in `KERNEL_FACTS.md`: `xe.probe_display=0`
  works (display IP and DMC load skipped) but reclaims **zero** VRAM — the
  nameplate-to-usable gap is GuC/GGTT/TTM, not display.
- **2026-09-15 — A770 backlogged by operator decision; B60 only from here.**
  The "A770 wedges under a carve" label is **withdrawn** as an over-claim. The
  cascade is now known (GGTT WARN -> GuC unresponsive -> GT reset -> forcewake
  `0xFFFFFFFF` -> reset -ETIMEDOUT -> wedged) and its root is a PCIe read getting
  no response, not a GPU-internal fault. Both cards carried the same overrunning
  carve; only the one behind the chipset switch chain fails. The deciding cell —
  GPU load on the A770 with no carve and arcwell unloaded — was never run and is
  backlogged with the card.

  Scope is now **B60 only**. This does not change the gate, which was measured on
  the B60 throughout, and it closes the "CPU pair on the A770" item as
  out-of-scope rather than impossible.
- **2026-09-15 — gate limitation 2 CLOSED: option (A) verified on the real artifact.**
  `results/GGUF_OPTION_A_2026-09-15.txt`. The real 40 GB UD-Q3_K_XL shard copied to
  `/flash`; 24 extents, ~1.7 GiB each, so a 2.4 MB slice straddles a boundary only
  ~0.14% of the time — the cell reads FIEMAP and places one slice across a boundary
  deliberately rather than sampling for it.

  Interior slice -> 1 segment. Straddling slice -> 2 segments, LBAs 280,928 sectors
  apart, reassembled at the right `dest_offset`s in one BO. Both byte-match a
  plain `pread`. `via_host_bounce=0`. Red case (partition offset dropped) fails on
  both slices.

  Found while verifying: `aw_ioc_stats.segments` stayed 0 through a run that moved
  3 segments — the batch path never updated it, so the field the uAPI documents as
  making gather visible was silent. Fixed.

  **Gate limitations now: 1 remains** — `B_P_host` against arcint's own staging
  ring rather than my equivalent. Limitation 3 (one card) is settled by the
  operator's decision to proceed on the B60 only; limitation 4 (short runs) is
  open but minor.
- **2026-09-15 — carve release: there is none, and it does not matter.**
  `pci_p2pdma_add_resource()` has no remove/release counterpart; the registration
  is devres-owned until device unbind. But the growth is **convergent**, not
  unbounded: the ceiling is all of usable VRAM, costing struct pages at ~1.56% of
  VRAM in host RAM (382 MiB for the B60's 23.9 GiB). That is housekeeping
  metadata, within the project's no-host-RAM rule.

  Implemented `carve_all=1` to pay it deterministically at load, which also
  removes the granule walk-down, section-conflict handling and `-ENOSPC` retries.
  It needs a BAR with no prior carves — with 160 MiB already registered it fails
  `-ENOMEM` in 13 ms on the overlap. Built, not yet validated on a clean BAR;
  folded into the next reboot rather than forcing one, since it is an optimisation
  and not on the gate's path.
- **2026-09-15 — clarification, because my own wording misled.** "Carve lifetime"
  and "cannot be released" read as though weights could be loaded but never
  unloaded. They cannot be read that way: a carve is a *registration of address
  ranges*, not an allocation. Demonstrated on a B60 with 160 MiB carved — three
  consecutive lifecycle runs all passed and the same physical VRAM addresses
  recurred across them, which only happens if the buffers were really freed and
  the memory reissued. arcwell also unloads and reloads cleanly with the carve
  present. The only thing that outlives a reboot-free session is host RAM for
  struct pages, bounded at 1.56% of VRAM.
- **2026-09-15 — per-expert LATENCY measured; it changes how arcwell must be used.**
  `results/LATENCY_2026-09-15.txt`. 64 sequential single-expert fetches: arcwell
  **1.125 ms**, host cold 1.547 ms, host warm 0.297 ms. arcwell wins cold-vs-cold.

  But the record prices the alternatives far lower: M14's CPU kernel computes an
  expert in **198 us**, and M9's overlapped upload costs ~45 us of critical path
  per expert. A synchronous arcwell fetch is ~5.7x the former and ~25x the latter.

  **Conclusion: arcwell is a throughput mechanism, not a latency one.** Its wins
  (1.84x bandwidth, 16.3x CPU) only pay off when the fetch is hidden behind
  compute. An integration that calls `AW_IOC_READ_BLOCKS` from the layer needing
  the expert would reproduce M9's original synchronous-upload defect with a worse
  constant. Depth is demonstrably the lever: 2.18 GB/s serial against 2.91 GB/s at
  `max_inflight=193`, on a card whose fabric is 79% idle.

  This is now the most important open question, and it is integration work rather
  than a bandwidth cell: does prefetch against a real routing trace hide 1.125 ms?
