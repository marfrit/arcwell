# arcwell M0 results — batch M0, BATCH-PENDING-REVIEW

Recorded observations only. No inference past what the hardware reported. Inbound-DMA acceptance remains untested; rung 3 stays gated.

## Experiment 1 — BAR aperture (recorded)
Target cards on the inference box, both on the xe driver:
- Arc A770: Physical Resizable BAR enabled. BAR 2 aperture = 16 GB (supported window 256 MB to 16 GB). Aperture maps effectively the full VRAM.
- Arc Pro B60 (Battlemage G21): Physical Resizable BAR present.

## Decision state (do not overclaim)
- Large resizable BAR aperture: RECORDED for the A770 (16 GB BAR2). The aperture-level precondition for inbound DMA into VRAM is met.
- Inbound DMA acceptance (does the bridge accept a write from the storage controller into the BAR window, and does it land in VRAM): NOT YET TESTED. Rung 3 remains gated on that experiment.
- Consequence: v1 stays rung 2 (pinned host buffer + OpenCL/OpenVINO copy) until the inbound-DMA experiment records a result. This matches the engineer brief.

## Experiment 2 — consumption path (pending)
Not yet run. The OpenCL/OpenVINO fast path is assumed per prior notes but must be confirmed before streaming into VRAM.

## Red-first acceptance cell (pending)
The failing cell (a test that fails if experiment 1 is not run or its result not recorded) is not yet written. It is the next deliverable for this batch.

## Cycle account
moved - M0 experiment 1 aperture recorded (A770 16 GB rebar, Battlemage rebar present, xe driver) toward the feasibility gate; row <M0-exp1-aperture> now <recorded>; row <M0-exp1-inbound-dma> now <untested, gated>; row <M0-exp2> now <pending>.
