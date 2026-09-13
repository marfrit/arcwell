# arcwell HANDOFF

Card: arcwell — direct-storage library for streaming mixture-of-experts weights into Arc GPU VRAM. First user: arcint.

## Leg 001 — card declaration (timestamp 2026-09-13T20:42:13Z)

Seats:
- Operator seat (human, owns remote, merges, tags).
- Engineer seat (builds and commits on dev; never pushes to master).
- Reviewer seat (classifies findings from a fresh archive; stated diff blob-hash verified before results are read; never edits code).
- Fix seat (implements fixes named by the reviewer).

V1 boundary: userspace, no kernel module. FIEMAP once at open -> pin the file -> read into a pinned host buffer -> OpenCL/OpenVINO copy into VRAM. Rung 3 (kernel module, NVMe-to-VRAM BAR direct) is a separate experiment gated on the BAR inbound-DMA test; not in v1.

First-user integration: arcint host-to-device slot upload path (backend_ov.cpp, 309 us per tensor baseline). The offload slot pool becomes a client of arcwell.

## Milestone roadmap

### M0 - feasibility gate (before any v1 code)
- Experiment 1: does the Arc GPU BAR accept inbound DMA from the NVMe controller into a large VRAM aperture? Result decides rung 2 (pinned host + copy) vs rung 3 (kernel module direct).
- Experiment 2: confirm the consumption path (OpenCL/OpenVINO) is solid; streaming into VRAM is worthless if the kernel cannot efficiently eat what landed.
- Acceptance (red-first): a test that fails if the BAR test is not run or its result not recorded.

### M1 - hello-world NVMe-to-VRAM
- Minimal direct-storage path: open one expert file, FIEMAP once, pinned host buffer, OpenCL/OpenVINO copy into VRAM, one copy.
- Acceptance (red-first): a cell that asserts the direct-read lands correct expert bytes in VRAM and is provably breakable (mutate the copy path, it goes red, then green at fix, raw output pasted both ways).

### M2 - expert-to-VRAM (arcint first user)
- arcwell API consumed by arcint: replace the host-to-device slot upload path; offload slot pool is a client of arcwell.
- Acceptance (red-first): benchmark cell that fails on the old copy path (309 us/tensor baseline) and passes on the new path; byte-exact equivalence preserved.
- Close-out per the move-forward rule: cycle account recorded.

## Repo note
Master is the frozen baseline (seed HOUSE_RULES.md). All work lands on dev marked BATCH-PENDING-REVIEW, per the convergence law.

## Leg 002 — batch M0, feasibility gate (announced 2026-09-13T21:07:22Z, released 2026-09-13T21:25:00Z)

Seat: Engineer (Claude). Cards touched: both, found idle (both serving units
inactive on arrival), left as found. Deliverables on dev, BATCH-PENDING-REVIEW:

- `M0_RESULTS.md` — the narrative record: lab as found, both experiments,
  the kernel-path reading, findings for the operator, cycle account.
- `M0_RESULTS.json` — the machine-readable twin the gate cells read; raw
  consumption logs embedded.
- `tests/test_m0_gate.py` — the red-first gate: four cells, each names its row.
- `tools/m0/consumption_probe.py` — experiment 2, one fresh process per card.
- `tools/m0/bar_inbound_dma_probe.py` — experiment 1's probe, staged, not run.

Stated diff for the reviewer: master `3d94de8` → this commit on dev; the
files above plus this HANDOFF section. Nothing else in the tree changed.

Gate run, red, before any result existed (2026-09-13T21:16:48Z):

```
ERROR tests/test_m0_gate.py::test_e1_bar_aperture_covers_vram - Failed: M0 no...
ERROR tests/test_m0_gate.py::test_e1_inbound_dma_probe_run_and_recorded - Fai...
ERROR tests/test_m0_gate.py::test_e1_v1_rides_rung2 - Failed: M0 not recorded...
ERROR tests/test_m0_gate.py::test_e2_consumption_verified_on_both_cards - Fai...
============================== 4 errors in 0.08s ===============================
```

Gate run after recording (2026-09-13T21:22:04Z):

```
tests/test_m0_gate.py::test_e1_bar_aperture_covers_vram PASSED           [ 25%]
tests/test_m0_gate.py::test_e1_inbound_dma_probe_run_and_recorded FAILED [ 50%]
tests/test_m0_gate.py::test_e1_v1_rides_rung2 PASSED                     [ 75%]
tests/test_m0_gate.py::test_e2_consumption_verified_on_both_cards PASSED [100%]
E       AssertionError: inbound DMA probe status is 'not_run', not 'run'
========================= 1 failed, 3 passed in 0.06s ==========================
```

Open row, and how it closes: run `tools/m0/bar_inbound_dma_probe.py` as root
on the host with both cards idle; paste its output into
`M0_RESULTS.json → experiment1.inbound_dma_probe.raw_log`, set `status: "run"`,
`outcome` to the observed value (`accepted` / `rejected_by_hardware` /
`refused_by_kernel`) and `rung3_physically_possible` to true only for
`accepted`. The cell then goes green on its own; no code change.

Findings the operator has to rule on before M1 (details in `M0_RESULTS.md` §4):
no expert file sits on a FIEMAP-capable filesystem (the NVMe carries only ZFS);
the A770 is capped by a shared chipset PCIe 3.0 x4 uplink (1.8 GB/s measured
under a running scrub).

Cycle account: moved — experiment 2 verified on both cards; rows `aperture`,
`decision`, `consumption` now green; gate installed red-first with both raw
runs pasted. exception — row `inbound DMA probe` did not move: executing the
probe needs root on the host and the session's tool policy refused it three
times, not worked around; cost: one operator command and a JSON edit; bought:
the kernel-path reading (xe exports VRAM to peer-to-peer dma-buf importers,
but no importer exists for an NVMe read in this kernel) and the ZFS/FIEMAP
finding M1 depends on.
