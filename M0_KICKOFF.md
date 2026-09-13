# arcwell M0 — engineer kickoff (batch M0, BATCH-PENDING-REVIEW)

You are the Engineer seat. This is batch M0. You are not writing v1 code yet. You are running the feasibility gate. Close-out with a cycle account per the move-forward rule.

## The gate
M0 decides whether arcwell v1 rides rung 2 (pinned host buffer + OpenCL/OpenVINO copy) or rung 3 (kernel module, NVMe-to-VRAM BAR direct). It also decides whether streaming into VRAM is worth anything at all.

## Experiment 1 — BAR inbound-DMA
Question: does the Arc GPU BAR accept inbound DMA from the NVMe controller into a large VRAM aperture?
Method:
- Record the BAR aperture size (resizable BAR state) for the target card.
- Attempt an inbound DMA write from the storage/NVMe controller into the BAR window; observe whether the GPU bridge accepts it and whether the bytes land in VRAM.
- Record the result as a single stated fact. Do not infer; record what the hardware did.
Decisions:
- Accepts inbound DMA into a large aperture: rung 3 is physically possible; note it but keep it out of v1.
- Does not accept: rung 3 is impossible; v1 is rung 2 (pinned host + copy). Record that.

## Experiment 2 — consumption path
Question: is the OpenCL/OpenVINO path solid enough that bytes landed in VRAM can be consumed efficiently?
- Confirm the fast compute path on the target cards (OpenCL via OpenVINO; SYCL is dead under xe, Vulkan is slow on Battlemage).
- If you cannot verify consumption, do not stream into VRAM. Record that.

## Red-first acceptance cell
Write the failing cell first: a test that fails if experiment 1 is not run or its result not recorded. Paste raw output. Then make it green by recording the result. A cell that cannot fail is decoration.

## Discipline
- No hostnames, addresses, credentials in tracked files.
- Work on dev, mark BATCH-PENDING-REVIEW, never touch master.
- Keep the diff stated and small; the reviewer reads from a fresh clone, blob hashes verified.

## Deliverable
Recorded results for both experiments + the red-first BAR-gate cell + a cycle account.
