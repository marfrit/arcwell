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
