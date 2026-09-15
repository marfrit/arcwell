# M4 API contract — arcwell-prefixed cuFile-mimicking shape (decision, v2)

## Layout (recorded)

Goal of this project is DMA from NVME to GPU VRAM of Intel Arc A770 and Intel Arc B60 for efficient LLM weight streaming. No host RAM Buffer - except for housekeeping of necessary metadata - is allowed

Both work. (A) experts as parts of one file: FIEMAP once for the whole file,
per-expert offset translated into the file's block extents; an expert can span
an extent boundary -> multi-segment DMA. (B) one file per expert, continuous
blocks: each expert usually ONE contiguous block range -> one contiguous DMA,
no extent-splitting; cost = register+FIEMAP per expert at load.
RECOMMENDATION: (B) is cleaner for direct block->VRAM. Caveat: contiguity is
best-effort (FIEMAP reports actual extents; ZFS may not guarantee contiguity).

## ioctl surface (recorded)
NO custom aw_set_fiemap ioctl. FIEMAP is the standard FS_IOC_FIEMAP ioctl
(already proven in M1); the module consumes its extents internally. The module's
custom ioctl surface is the DMA-mapping/registration path (GDS-model engine):
aw_map_buffer (register a VRAM buffer as a DMA target for the NVMe controller)
+ aw_read_blocks (issue storage->VRAM DMA). Pure raw-block model: FIEMAP drops
out entirely; ioctls take raw block ranges.

## FIEMAP resolution (recorded)
The direct path is RAW NVMe BLOCK -> VRAM. FIEMAP is a filesystem ioctl
(file logical extents -> physical blocks); it has NO place in a raw-block
direct DMA data path. Roles:
- experts addressed as raw NVMe blocks: FIEMAP ABSENT entirely — the read ioctl
  takes a block range (offset/length in NVMe blocks), DMA straight into VRAM.
- experts addressed as files: FIEMAP happens ONCE at open (file->block-range
  translation), then the streaming data path is block-addressed. NEVER per-read.
So FIEMAP is at most one open-time translation, never in the streaming loop.

## Primary use case (recorded)
EXPERT STREAMING TO VRAM FOR INFERENCE (MoE): read expert-weight buffers from
storage into VRAM, one-way, on the inference hot path. The read path is the
API's core; write-back is not on the inference path.

## Decision
The module's contract keeps cuFile's SHAPE (one call: storage -> VRAM buffer)
with NO CUDA/nv_ naming and NO CUDA dependency (compute is OpenCL/Level-Zero).
The device-pointer analog is a module buffer handle from AW_IOC_MAP_BUFFER, not
a CUDA pointer, and not a client-library object.

The contract lives in the uAPI, not in a client library:
`stub/include/aw_uapi.h` -- `AW_IOC_MAP_BUFFER` (register a VRAM buffer as an
NVMe DMA target), `AW_IOC_READ_BLOCKS` (issue storage->VRAM DMA over a raw block
range), `AW_IOC_STATS` (so a bounce cannot hide).

Streaming shape: register the buffer set once at load, then one
`AW_IOC_READ_BLOCKS` per expert weight buffer. A batch of experts is the
inference pattern, not a single bulk read. Any thin client that wraps these
ioctls exists only to issue them; it is not a data path and holds no bytes.

> An earlier revision of this file expressed the same contract as a client
> library. That was a userspace data path and is not the architecture; it has
> been removed, and its surviving decisions are above and in the uAPI.

## Honest scope note (updated at 0.0.1)
The API CONTRACT is the easy, cosmetic part -- it is a thin shape. The hard part
is the KERNEL DMA ENGINE underneath (what makes AW_IOC_READ_BLOCKS actually do
storage->VRAM without the host bounce). That is device-specific Intel xe +
NVMe/storage-driver integration (GDS model), NOT a CUDA mimic.

**It is no longer unbuilt.** `stub/src/arcwell.c` implements it and `results/`
carries the measurements. The warning above still stands as a warning: the shape
was a day's work and the engine was the rest. Mimicking the API is still not
mimicking GDS -- see `docs/USING_ARCWELL.md` §8 for what remains unproven.
