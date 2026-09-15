# M4 — stub wiring reference (extracted from ssd-gpu-dma, BSD-2-Clause)

## Port target: the NVMe-controller -> GPU-memory DMA wiring

Goal of this project is DMA from NVME to GPU VRAM of Intel Arc A770 and Intel Arc B60 for efficient LLM weight streaming. No host RAM Buffer - except for housekeeping of necessary metadata - is allowed

Source (refs/ssd-gpu-dma): a Linux kernel module exposing an NVM controller.

## Wiring path (extractable)
1. module/ctrl.c: register the NVMe controller as a PCI device; expose via
   char device + ioctl/RPC (nvm_rpc).
2. include/nvm_ctrl.h: nvm_dis_ctrl_map_p2p_device / unmap — maps a PCIe
   peer-to-peer device (the GPU) as a DMA target. THE core P2P wiring.
3. include/nvm_dma.h: DMA alloc/map (nvm_dma_alloc / nvm_dma_map).
4. examples/read-blocks/read.c: the ioctl CALLER — open the module, map the GPU
   P2P device, read NVMe blocks, DMA-write into GPU memory. Thin client only; it
   is not a data path and holds no bytes.

## arcwell stub = mirror this into the arcwell shape
- Use xe host-visible VRAM (Xe_BO_FLAG_HOST_VRAM) as the GPU memory target.
- Use kernel P2PDMA (drivers/pci/p2pdma.c) for the NVMe<->GPU peer link.
- NVMe controller DMA-writes the "hello world" into VRAM; GPU readback check.
- License: BSD-2-Clause permits legal port of this wiring.

## What arcwell adds beyond the demo
- B60/Battlemage: expected-same (same xe memory model), confirm on hardware.
- Robustness: error paths, extent-list (SGL) support for non-contiguous blocks.
- The 3-axis benchmark (host RAM freed / copy latency removed / throughput).
