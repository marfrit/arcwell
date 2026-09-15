/* arcwell stub — DMA-into-VRAM ioctl interface (port target)
 * Mirrors the ssd-gpu-dma wiring: map GPU VRAM as a P2P device target,
 * NVMe controller DMA-writes a pattern, GPU readback verifies.
 * License: BSD-2-Clause (port from ZaidQureshi/fixed--ssd-gpu-dma).
 */
#ifndef ARCWELL_DMA_H
#define ARCWELL_DMA_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define ARCWELL_DMA_MAGIC 'A'

/* map GPU VRAM (xe host-visible BO) as the P2P DMA target */
struct arcwell_dma_map {
	__u32 bo_handle;   /* xe BO handle (XE_BO_FLAG_VRAM|NEEDS_CPU_VISIBLE) */
	__u32 p2p_flags;
	__u64 host_addr;   /* host-visible PCIe address of the BO (out) */
};

/* NVMe controller DMA-writes pattern into the mapped VRAM */
struct arcwell_dma_write {
	__u64 host_addr;   /* where the NVMe DMA writes */
	__u64 len;
	__u64 pattern;     /* "hello world" pattern word */
};

#define ARCWELL_DMA_MAP   _IOWR(ARCWELL_DMA_MAGIC, 1, struct arcwell_dma_write)
#define ARCWELL_DMA_WRITE _IOW(ARCWELL_DMA_MAGIC, 2, struct arcwell_dma_write)

#endif
