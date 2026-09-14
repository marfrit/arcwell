# arcwell — session handover

You are in arcwell: a direct-storage library for streaming mixture-of-experts weights into Arc GPU VRAM. First user: arcint. You are the Engineer seat.

## Governance (read HOUSE_RULES.md — encoded from mneme 379-386)
- Seats: Operator (human) owns remote and merges; Engineer (you) builds and commits on dev, never pushes master; Reviewer classifies findings from a fresh archive, never edits code; Fix seat implements reviewer-named fixes.
- Convergence: work lands on dev marked BATCH-PENDING-REVIEW; stated diff, blob-hash verified before results are read.
- Move-forward: every close-out ends with a cycle account (moved - <what> toward <gate>, row <which> now <state>; or exception - <why>, <cost>, <what it bought>).
- Red-first: acceptance cells run at a high level and are provably able to fail; mutate, red naming the cell, green at fix, raw output pasted both ways. A cell that cannot fail is decoration.
- Hygiene: no hostnames, addresses, or credentials in tracked files.

## State (all pushed, clean)
- master (frozen baseline): HOUSE_RULES.md, HANDOFF.md (leg 001, M0/M1/M2), ENGINEER_PROMPT.md (gargantuan brief).
- dev: M0_KICKOFF.md + M0_RESULTS.md (batch M0, BATCH-PENDING-REVIEW).

## Determined facts
- Hardware (target box, xe driver): Arc A770 — Resizable BAR, BAR2 = 16 GB aperture (full VRAM mapped); Arc Pro B60 / Battlemage G21 — Resizable BAR present.
- The aperture-level precondition for inbound NVMe DMA into VRAM is met, but inbound-DMA acceptance is untested; rung 3 stays gated. v1 = rung 2 (pinned host buffer + OpenCL/OpenVINO copy; FIEMAP once, one copy, no page-cache bounce).
- Compute reality: SYCL is dead under xe (memcpy abort); Vulkan is slow on Battlemage; OpenCL/OpenVINO is the only fast path; consumption and fencing ride OpenCL/OpenVINO, not Level Zero.

## Open work (in order)
1. M0 experiment 1 completion: the red-first BAR-gate cell (fails if the inbound-DMA experiment is not run or recorded), then attempt a storage-controller write into the A770 BAR window and record whether it lands in VRAM; decides rung 2 vs rung 3.
2. M0 experiment 2: confirm the OpenCL/OpenVINO consumption path is solid before streaming into VRAM.
3. M1: hello-world NVMe-to-VRAM (one expert file, FIEMAP once, pinned host, one OpenCL copy) plus a provably breakable VRAM-landing cell.
4. M2: arcint first-user integration; replace the host-to-device slot upload in backend_ov.cpp (baseline 309 us per tensor, 13.1 s of gpu_copy per 16-token window); benchmark cell fails old path, passes new; byte-exact equivalence preserved.

## Discipline
Work on dev, never touch master; keep diffs small and stated; measure against the 309 us per tensor baseline; paste raw red and green output; close with a cycle account. Do not overclaim; record what the hardware did, not what you inferred.
