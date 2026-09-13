# arcwell M0 — feasibility gate, recorded results

Batch M0, HANDOFF leg 002, Engineer seat. Recorded 2026-09-13. The machine-
readable twin of this file is `M0_RESULTS.json`; the gate cells in
`tests/test_m0_gate.py` read only the JSON. Every number below names the card,
the configuration and the raw log it came from. No hostnames or addresses are
recorded anywhere in this tree; PCI bus/device/function numbers are hardware
positions, not network addresses.

## 0. The lab as found (the target inference box)

- Host kernel 7.0.14 (distribution-patched), xe driver as a DKMS module
  (`xe-ringorder 7.0.14+p1`, the ring-order backport). `CONFIG_PCI_P2PDMA=y`,
  `CONFIG_DMABUF_MOVE_NOTIFY=y`, `CONFIG_ZONE_DEVICE=y`, `CONFIG_IO_URING=y`.
- AMD IOMMU enabled, every group in `DMA-FQ` (lazy) translation; kernel
  lockdown `none` (sysfs PCI resource files are mappable by root).
- Both arcint units were **inactive on arrival**, the cards free; they were left
  as found (not started).
- The GPUs are reached from a privileged container that sees `/dev/dri` and
  the host's PCI sysfs; the host runs no display on either card.

### Cards and their PCIe position

| card | BDF | path from the root complex | payload link (switch upstream port) | endpoint link register |
|---|---|---|---|---|
| Arc Pro B60 (GPU.0) | 0000:0f:00.0 | root port 00:03.1 → on-board switch 0d:00.0/0e:01.0 | 16 GT/s x8 (PCIe 4.0 x8) | 2.5 GT/s x1 |
| Arc A770 (GPU.1) | 0000:06:00.0 | root port 00:01.2 → chipset 02:00.2 → 03:00.0 → on-board switch 04:00.0/05:01.0 | 8 GT/s x4 (PCIe 3.0 x4, "downgraded") | 2.5 GT/s x1 |

The endpoint's own `current_link_speed`/`current_link_width` read 2.5 GT/s x1
on both cards for the whole of a 14 GB/s copy (see §2): the link behind each
card's on-board switch is not the payload link. Read the switch's upstream port
for the number that matters.

The A770 shares the chipset's single PCIe 3.0 x4 uplink with the SATA
controller (five 4 TB HDDs, the model-store pool) and three Ethernet NICs.

### Storage

- One NVMe: Samsung SM961 256 GB, 0000:01:00.0, PCIe 3.0 x4 on CPU root port
  00:01.1, IOMMU group 0 (shared with the A770's root port), kernel driver `nvme`.
- Its three partitions are all ZFS members: 16 GB SLOG and 32 GB L2ARC for the
  HDD pool, and a 190 GB pool of its own (`flash`) that carries one GGUF model.
- The MoE serving artifacts (`/models/ov/...`) live on the HDD pool: raidz2 over
  five 4 TB SATA disks, compression on, 128 KiB recordsize, exposed to the
  container as a bind mount. A monthly scrub was running throughout this leg
  (~0.5 GB/s issued).
- **There is no non-ZFS filesystem on the NVMe and no expert file on a
  FIEMAP-capable filesystem.** OpenZFS does not implement the FIEMAP ioctl, and
  its on-disk blocks are compressed and checksummed, so "FIEMAP once at open →
  read by LBA" has no substrate here. This is an M1 precondition, recorded for
  the operator (see §4).

## 1. Experiment 1 — BAR inbound DMA

### Row "aperture" — recorded (cell `test_e1_bar_aperture_covers_vram`, green)

| card | physical VRAM | CPU-accessible VRAM (excl. stolen) | BAR2 current | ReBAR sizes offered |
|---|---|---|---|---|
| Arc Pro B60 | 0x600000000 (24 GiB) | 0x5fa000000 | 32 GiB | 256 MiB … 32 GiB |
| Arc A770 | 0x400000000 (16 GiB) | 0x3fa000000 | 16 GiB | 256 MiB … 16 GiB |

Source: `lspci -vv` Physical Resizable BAR capability and the xe probe lines
(`VISIBLE VRAM`, `VRAM[0]: ... CPU accessible size`). Both cards expose the
whole of VRAM through a single large aperture. `xe.vram_bar_size=0` (driver
default; the firmware already set the maximum).

### Row "kernel path" — read off the installed xe source (not a measurement)

`xe/xe_dma_buf.c` in the installed DKMS tree:

- `xe_dma_buf_attach`: an attachment is `peer2peer` if
  `pci_p2pdma_distance(gpu, importer) >= 0`; the exporter declares
  `.allow_peer2peer = true`.
- `xe_dma_buf_pin`: the BO stays in VRAM only when **every** attachment is
  `peer2peer` and `CONFIG_DMABUF_MOVE_NOTIFY` is set; otherwise it is migrated
  to TT (system memory) before pinning.
- `xe_dma_buf_map`: for a VRAM-resident BO the scatter table comes from
  `xe_ttm_vram_mgr_alloc_sgt(..., attach->dev, ...)`, i.e. bus addresses inside
  BAR2 for the importer.

So the exporter half of rung 3 exists in this xe: a P2P-capable in-kernel
importer is handed VRAM bus addresses. What does **not** exist is an importer
that lets an NVMe read land in such a dma-buf from userspace:

- no P2P memory provider is registered (`/sys/bus/pci/devices/*/p2pmem` is
  absent everywhere; xe does not call `pci_p2pdma_add_resource`);
- this kernel's io_uring has dma-buf only for zero-copy network receive
  (`IORING_ZCRX_AREA_DMABUF`), not for registered buffers used by block reads.

Rung 3 therefore remains what the roadmap says it is: a kernel-module
experiment, outside v1.

### Row "inbound DMA probe" — NOT RUN (cell `test_e1_inbound_dma_probe_run_and_recorded`, red)

The probe is `tools/m0/bar_inbound_dma_probe.py`: an `O_DIRECT` `preadv` from
NVMe LBA 0 (the GPT header, read-only) into an `mmap` of each card's
`resource2` at a free offset (16 GiB into the B60, 8 GiB into the A770), with a
CPU read-back of the 4 KiB block before and after and a control read into an
anonymous host page. It records one of three outcomes per card: `accepted`
(bytes landed in VRAM), `rejected_by_hardware` (the read completed or errored
but nothing landed), `refused_by_kernel` (the syscall failed before any DMA).

It was not run in this leg. Executing it needs root on the host (PCI resource
mapping and the raw block device) and the session's tool-permission policy
refused that action three times (write-and-run, copy-and-run, copy alone). The
engineer seat did not work around the refusal. The row stays red until the
operator runs the probe and pastes its log into
`M0_RESULTS.json → experiment1.inbound_dma_probe.raw_log`, setting `status`
to `run`, `outcome` to the observed value and `rung3_physically_possible`
accordingly.

Expectation, stated as expectation and not as result: `pin_user_pages` refuses
`VM_PFNMAP` mappings, so the kernel should return `EFAULT` before a single TLP
is issued (`refused_by_kernel`). A userspace probe can only confirm or refute
that boundary; the hardware answer proper needs the kernel-module experiment.

### Row "decision" — recorded (cell `test_e1_v1_rides_rung2`, green)

v1 rides rung 2 (pinned host buffer + OpenCL/OpenVINO copy) in every branch of
the kickoff's decision table. Whether rung 3 is physically possible is left
unrecorded until the probe row turns green.

## 2. Experiment 2 — consumption path (measured, both cards)

Method (`tools/m0/consumption_probe.py`, one fresh process per card): 256 MiB
of pseudo-random bytes are landed in VRAM through OpenVINO's remote-tensor API
(OpenCL USM device buffer); a compiled GPU model (`Convert u8→i32`, `ReduceSum`
over rows of 4096) consumes every byte and its 65,536 partial sums are compared
exactly against numpy. Three landings are timed, each fenced by the inference
that reads it: pageable host → device, USM host (pinned) → device, and the
kernel reading the USM host buffer in place over PCIe. Then one byte is flipped
on the host, re-landed, and exactly one partial must move; finally the device
tensor is copied back and compared byte-for-byte.

Configuration: OpenVINO 2026.4.0 (the tooling venv), Intel compute runtime
NEO 26.27.39122.11, both cards idle (no serving unit running), the HDD-pool
scrub issuing ~0.5 GB/s over the chipset uplink the A770 shares. Best of three.

| card (payload link) | pageable → device | pinned (USM host) → device | kernel reads USM host in place | infer, 256 MiB resident |
|---|---|---|---|---|
| Arc Pro B60 (PCIe 4.0 x8) | 19.80 ms, 13.56 GB/s | 18.73 ms, 14.33 GB/s | 25.69 ms, 10.45 GB/s | 6.3 ms |
| Arc A770 (PCIe 3.0 x4 via chipset) | 150.95 ms, 1.78 GB/s | 146.61 ms, 1.83 GB/s | 152.71 ms, 1.76 GB/s | 9.4 ms |

Exactness: all landings exact on both cards; mutation moved only row 32768 on
both cards; device → host read-back byte-exact on both cards. Raw logs in
`M0_RESULTS.json → experiment2.cards.*.raw_log`.

Readings:

- The B60 copies at 91 % of PCIe 4.0 x8's 15.75 GB/s. Pinned beats pageable by
  6 %. The consumption path is solid: `consumption_verified = true`.
- The A770 lands bytes at 1.8 GB/s, under half of even its Gen3 x4 ceiling
  (3.94 GB/s raw). That is topology, not mechanism: it sits behind the
  chipset's shared x4 uplink and this run coincided with a scrub on the same
  link. Re-measure with the scrub paused before any M2 number is claimed on
  this card; on this card the copy will be the bound whatever arcwell does.
- Reading USM host memory in place costs the B60 27 % against a device-resident
  copy and costs the A770 nothing measurable; both are viable consumption modes
  for rung 2.

Red-first note: run 1 of the mutation cell went red on both cards
("rows whose partial moved: []") because `ov.Tensor(array)` copies the numpy
array and the flipped byte never reached the device — a blind fill, the harness
not the hardware. `shared_memory=True` fixed it; run 2 is the recorded one.
Both raw outputs:

```
run 1 (both cards): mutation: flipped byte at row 32768 col 7; rows whose partial moved: []; matches mutated expectation: False
                    AssertionError: mutation did not move exactly one partial
run 2 (both cards): mutation: flipped byte at row 32768 col 7; rows whose partial moved: [32768]; matches mutated expectation: True
```

## 3. Gate cell runs (raw)

Red, before any result existed in the tree (2026-09-13T21:16:48Z):

```
== RED RUN (no M0_RESULTS.json in tree) 2026-09-13T21:16:48Z
ls: cannot access 'M0_RESULTS.json': No such file or directory
E           Failed: M0 not recorded: M0_RESULTS.json is absent from the tree
ERROR tests/test_m0_gate.py::test_e1_bar_aperture_covers_vram - Failed: M0 no...
ERROR tests/test_m0_gate.py::test_e1_inbound_dma_probe_run_and_recorded - Fai...
ERROR tests/test_m0_gate.py::test_e1_v1_rides_rung2 - Failed: M0 not recorded...
ERROR tests/test_m0_gate.py::test_e2_consumption_verified_on_both_cards - Fai...
============================== 4 errors in 0.08s ===============================
```

After recording (see the run pasted in HANDOFF leg 002): three cells green,
`test_e1_inbound_dma_probe_run_and_recorded` red naming the unrun probe.

## 4. Findings for the operator (not decisions)

1. **M1's substrate does not exist yet.** No expert file lives on a
   FIEMAP-capable filesystem; the NVMe carries only ZFS. Options, none chosen
   here: an ext4/xfs zvol on the NVMe pool (FIEMAP works, LBAs are the zvol's,
   which suffices for rung 2 but never for rung 3); a spare partition; or
   dropping FIEMAP from v1 (an `O_DIRECT` reader into pinned memory needs no
   extent map — FIEMAP only pays for a kernel-direct path).
2. **The A770 is bandwidth-starved by position**, not by software. Any M2 win
   on the coder card is capped by a shared PCIe 3.0 x4 uplink.
3. **Rung 3 has an exporter but no importer** in this kernel; the kernel-module
   experiment is the only way to make the hardware answer.

## 5. Cycle account (move-forward rule)

moved — experiment 2 verified on both cards; rows `aperture`, `decision`,
`consumption` now green; M0 gate cell installed red-first with both raw runs.
exception — row `inbound DMA probe` did not move: the probe is written and
staged in `tools/m0/` but executing it needs root on the host, which this
session's tool policy refused; cost: one operator command and a JSON edit;
what it bought: the kernel-path reading that bounds what any userspace probe
can show, and the FIEMAP/ZFS finding that M1 must resolve before its first
line of code.
