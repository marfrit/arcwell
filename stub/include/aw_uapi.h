/* SPDX-License-Identifier: BSD-2-Clause */
/* aw_uapi.h — arcwell kernel-module uAPI.
 *
 * arcwell is a KERNEL MODULE project. The card's core is real NVMe bytes
 * landing in Arc VRAM with nothing else in the loop, which requires a
 * kernel-side DMA path; a userspace data path cannot provide it (see
 * KERNEL_FACTS.md, "Architecture"). The prior art in refs/ssd-gpu-dma is
 * likewise a Linux kernel module.
 *
 * Contract source: M4_API.md. The API's shape is one call, storage -> VRAM,
 * mimicking cuFile's SHAPE with no CUDA naming or dependency. Per that record,
 * the module's custom surface is the DMA-mapping/registration path:
 *
 *   AW_IOC_MAP_BUFFER    register a VRAM buffer as a DMA target for the NVMe
 *                        controller
 *   AW_IOC_READ_BLOCKS   issue storage -> VRAM DMA over a raw block range
 *
 * FIEMAP is NOT part of this surface. It is the standard FS_IOC_FIEMAP ioctl
 * and, where experts are addressed as files, it runs once at open as a
 * file->block-range translation. It never appears in the streaming loop, and
 * there is no aw_set_fiemap ioctl (M4_API.md, held by tests/test_m4_api.py).
 *
 * STATUS at 0.0.1: BUILT and measured. stub/src/arcwell.c serves this surface;
 * results/ carries the raw output, red case included.
 *
 * The blockers this header once listed are resolved, and two of them were never
 * real (KERNEL_FACTS.md has the disproofs):
 *   B1 "pci_p2pdma_add_resource deadlocks on Arc BAR2" -- an ABI bug in a
 *      locally-declared extern, not a kernel limit. Returns 0 in 9 ms.
 *   B2 "a BAR dma_addr is refused by the NVMe path" -- never actually tested.
 *      dma_map_resource() returns a valid IOMMU-translated address.
 *   B3 host-visible VRAM BOs work on stock xe. Unchanged, and still the recipe.
 *
 * What remains unproven is stated in docs/USING_ARCWELL.md §8, not hidden here.
 */
#ifndef AW_UAPI_H
#define AW_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define AW_DEVICE_NAME	"arcwell"
#define AW_IOC_MAGIC	'W'

/* How the caller describes the destination, and how the module obtained it.
 * The module must not accept a bare user pointer as a DMA target, and must not
 * accept a host page as a device destination: a host buffer wearing a VRAM
 * label is how a zero-copy claim becomes false without anyone deciding it. */
enum aw_buffer_source {
	AW_BUF_NONE = 0,
	/* an xe GEM handle, mapped and pinned by the module */
	AW_BUF_XE_GEM = 1,
	/* a dma-buf fd exported by xe; the module attaches and maps it and uses
	 * the exporter's sg dma addresses (BAR2 + offset) */
	AW_BUF_DMABUF = 2,
};

/* Flags for aw_ioc_map_buffer.flags */
#define AW_MAP_F_REQUIRE_P2P	(1u << 0)	/* refuse to register unless the
						 * mapping really is peer-to-peer
						 * DMA-capable; never silently
						 * fall back to a host bounce */

/**
 * struct aw_ioc_map_buffer - AW_IOC_MAP_BUFFER
 * @in_handle:	xe GEM handle or dma-buf fd, per @in_source
 * @in_source:	enum aw_buffer_source
 * @in_length:	bytes to register; must be a multiple of the NVMe logical block
 * @out_handle:	module handle used by AW_IOC_READ_BLOCKS
 * @out_flags:	AW_MAP_F_* bits the module actually honours. If the mapping is
 *		not peer-capable the ioctl FAILS; it does not report a weaker
 *		mapping as success.
 *
 * Returns 0, or -EOPNOTSUPP when the direct DMA path is not available (with
 * the reason in dmesg naming which blocker was hit), or -EINVAL.
 */
struct aw_ioc_map_buffer {
	__u64 in_handle;
	__u32 in_source;
	__u32 in_length;
	__u32 out_handle;
	__u32 out_flags;
};

/* Flags for aw_ioc_read_blocks.flags */
#define AW_READ_F_REVALIDATE	(1u << 0)	/* re-derive block map before the
						 * DMA; a COW filesystem can move
						 * blocks under a cached map */

/**
 * struct aw_ioc_read_blocks - AW_IOC_READ_BLOCKS
 * @in_buffer_handle:	from AW_IOC_MAP_BUFFER
 * @in_start_block:	NVMe logical block number (512 B units unless the
 *			namespace reports otherwise)
 * @in_block_count:	logical blocks to transfer into the registered buffer
 * @in_dest_offset:	byte offset within the registered buffer
 * @in_flags:		AW_READ_F_*
 * @out_bytes:		bytes actually transferred by the controller
 * @out_segments:	SGL/PRP segments used; nonzero count means the extents
 *			were gathered, because contiguity is best-effort and is
 *			never assumed (M4_STORAGE_ZFS)
 *
 * This is the primary call: one expert weight buffer per call. Returns bytes
 * transferred or negative errno. -ESTALE if @AW_READ_F_REVALIDATE found the
 * block map moved; -EOPNOTSUPP if the registered buffer is not DMA-attached.
 */
struct aw_ioc_read_blocks {
	__u64 in_buffer_handle;
	__u64 in_start_block;
	__u64 in_block_count;
	__u64 in_dest_offset;
	__u32 in_flags;
	__u32 _pad;
	__u64 out_bytes;
	__u32 out_segments;
	__u32 _pad2;
};

/* Upper bound on one batch, so a client cannot make the kernel allocate without
 * limit. Chosen to comfortably exceed a MoE layer's active expert count. */
#define AW_BATCH_MAX 256

/**
 * struct aw_ioc_read_batch - AW_IOC_READ_BATCH
 * @in_requests:  user pointer to @in_count struct aw_ioc_read_blocks
 * @in_count:     number of requests, 1..AW_BATCH_MAX
 * @out_bytes:    total bytes transferred across the batch
 * @out_completed: requests that completed without error
 * @out_err_index: index of the first failing request, when @out_err is nonzero
 * @out_err:      0, or the negative errno of the first failure
 *
 * WHY THIS EXISTS. M4_API.md states the inference pattern plainly: "A batch of
 * experts is the inference pattern, not a single bulk read." AW_IOC_READ_BLOCKS
 * issues one transfer and waits for it, so a batch of N experts costs N
 * round-trips end to end and the NVMe queue never holds more than one request.
 * This call submits the whole batch and then waits, so the controller sees the
 * requests concurrently and its queue depth is actually used.
 *
 * Every request is still one expert buffer; this changes how they are submitted,
 * not what a transfer is. Per-request semantics, including the peer-to-peer
 * requirement, are unchanged.
 *
 * Returns 0 if every request succeeded, or a negative errno. A partial failure
 * still returns the counts, so the caller can tell what landed.
 */
struct aw_ioc_read_batch {
	__u64 in_requests;
	__u32 in_count;
	__u32 in_flags;
	__u64 out_bytes;
	__u32 out_completed;
	__u32 out_err_index;
	__s32 out_err;
	__u32 _pad;
};

#define AW_IOC_READ_BATCH _IOWR(AW_IOC_MAGIC, 5, struct aw_ioc_read_batch)

/**
 * struct aw_ioc_batch_submit - AW_IOC_SUBMIT_BATCH
 * @in_requests:  user pointer to @in_count struct aw_ioc_read_blocks
 * @in_count:     1..AW_BATCH_MAX
 * @out_batch_id: opaque id, passed to AW_IOC_BATCH_WAIT
 * @out_submitted: requests whose transfers were queued
 * @out_err:      submission-time error only; transfer errors surface at wait
 *
 * WHY THIS EXISTS, AND WHY AW_IOC_READ_BATCH IS NOT ENOUGH. Measured on this
 * hardware (results/LATENCY_2026-09-15.txt): one expert fetched synchronously
 * costs **1.125 ms**, against 198 us to compute that expert on the host CPU
 * (docs/milestone-0.3.0.md M14) and ~45 us of critical path for an overlapped
 * upload (M9). On the decode hot path, unprefetched, a synchronous fetch loses.
 *
 * arcwell's advantages are throughput ones — 1.84x the cold host path's bandwidth
 * and 16.3x less CPU per GiB — and throughput advantages only pay off when the
 * latency is hidden. That requires issuing fetches AHEAD of the layer that needs
 * them, which a blocking call cannot express.
 *
 * This is cuFile's shape: cuFileBatchIOSubmit returns as soon as the I/O is
 * queued and cuFileBatchIOGetStatus collects it. GPUDirect split them for the
 * same reason.
 *
 * Returns 0 once the batch is queued. Buffer references are held until the batch
 * is collected, so a buffer cannot be unmapped out from under an in-flight
 * transfer. Outstanding batches are drained when the file descriptor closes.
 */
struct aw_ioc_batch_submit {
	__u64 in_requests;
	__u32 in_count;
	__u32 in_flags;
	__u64 out_batch_id;
	__u32 out_submitted;
	__s32 out_err;
};

#define AW_IOC_SUBMIT_BATCH _IOWR(AW_IOC_MAGIC, 6, struct aw_ioc_batch_submit)

/**
 * struct aw_ioc_batch_wait - AW_IOC_BATCH_WAIT
 * @in_batch_id:  from AW_IOC_SUBMIT_BATCH
 * @in_timeout_us: 0 = poll and return -EAGAIN if not finished;
 *                 UINT64_MAX = block until done; otherwise wait that long
 * @out_bytes:    bytes transferred
 * @out_completed: requests that completed
 * @out_segments: bios the transfers were split into (gather made visible)
 * @out_err:      0, or the first transfer error
 *
 * On success the batch is collected and its id becomes invalid. On -EAGAIN the
 * batch is still in flight and the id remains valid.
 */
struct aw_ioc_batch_wait {
	__u64 in_batch_id;
	__u64 in_timeout_us;
	__u64 out_bytes;
	__u32 out_completed;
	__u32 out_segments;
	__s32 out_err;
	__u32 _pad;
};

#define AW_IOC_BATCH_WAIT _IOWR(AW_IOC_MAGIC, 7, struct aw_ioc_batch_wait)

/**
 * AW_IOC_UNMAP_BUFFER - release a buffer registered by AW_IOC_MAP_BUFFER
 * @arg: the out_handle returned by AW_IOC_MAP_BUFFER
 *
 * Unpins and detaches the dma-buf and drops the module's reference to it. The
 * caller's own dma-buf fd is unaffected.
 *
 * ADDED AFTER THE ORIGINAL CONTRACT. M4_API.md specified map + read + stats and
 * no release call. That was an omission, not a design: a registration surface
 * with no way to unregister leaks a pinned VRAM buffer per call, and expert
 * streaming registers continuously. Buffers are also released automatically when
 * the file descriptor is closed, so a crashing client cannot strand VRAM.
 *
 * Returns 0, or -EINVAL if the handle is not registered to this open file, or
 * -EBUSY if a transfer against it is still in flight.
 */
#define AW_IOC_UNMAP_BUFFER _IOW(AW_IOC_MAGIC, 4, __u32)

/* Observability, so a slow path cannot hide behind a working-looking call. */
struct aw_ioc_stats {
	/* --- original layout, DO NOT REORDER. New fields are APPENDED below.
	 * AW_IOC_STATS is _IOR(..., struct aw_ioc_stats), so the struct size is
	 * encoded in the ioctl number: changing the size changes the command, and a
	 * client built against an older header gets -ENOTTY rather than silently
	 * misreading fields. That is the desired failure, but it still breaks the
	 * client, so grow this struct only at the end and rebuild callers. --- */
	__u64 reads;
	__u64 bytes;
	__u64 us_total;
	__u32 via_host_bounce;	/* Counts transfers that WOULD have gone through
				 * system RAM and were refused. MUST stay 0.
				 * Nonzero means a caller asked for something the
				 * module could only have served by bouncing --
				 * the request failed, but the configuration that
				 * produced it is wrong.
				 * Incremented at two sites in arcwell.c: the
				 * exporter clearing attach->peer2peer, and an sg
				 * whose pages are not P2PDMA pages. There is no
				 * path that bounces WITHOUT incrementing this,
				 * because there is no path that bounces at all --
				 * both sites return an error. */
	__u32 segments;

	/* --- appended 2026-09-15: buffer lifecycle --- */
	__u32 buffers_live;	/* registered right now by this open file */
	__u32 buffers_peak;	/* high-water mark; if this only ever grows, the
				 * client is leaking registrations */

	/* --- appended 2026-09-15: batched submission --- */
	__u64 batches;		/* AW_IOC_READ_BATCH calls */
	__u64 batch_reads;	/* requests submitted through batches */
	__u32 max_inflight;	/* largest number of bios in flight at once; if this
				 * stays at 1 the queue depth is not being used */
	__u32 batches_inflight;	/* submitted but not yet collected */
};

#define AW_IOC_MAP_BUFFER   _IOWR(AW_IOC_MAGIC, 1, struct aw_ioc_map_buffer)
#define AW_IOC_READ_BLOCKS  _IOWR(AW_IOC_MAGIC, 2, struct aw_ioc_read_blocks)
#define AW_IOC_STATS        _IOR(AW_IOC_MAGIC,  3, struct aw_ioc_stats)
/* AW_IOC_UNMAP_BUFFER is defined above, next to its documentation. */

#endif /* AW_UAPI_H */
