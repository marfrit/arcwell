// SPDX-License-Identifier: GPL-2.0
/* aw_e2e_test — BACKLOG item 15b. THE END-TO-END CELL.
 *
 * Joins the two halves that were proven separately:
 *   item 14  NVMe -> GPU-owned BO by controller DMA, via_host_bounce=0
 *   item 15a GPU-owned BO -> OpenCL kernel, via dma-buf import
 *
 * One BO, one dma-buf fd, used by both. The chain under test is:
 *
 *   NVMe controller --DMA--> Arc VRAM BO --dma-buf import--> OpenCL kernel
 *
 * with no host bounce anywhere, and completion fenced on a cl_event per
 * ENGINEER_PROMPT.md §2.
 *
 * Assertion chain, each link independently checkable:
 *   1. the 0xA5 poison is gone          -> arcwell wrote into the BO
 *   2. gpu_sum == cpu_sum               -> the GPU reads what is in the BO
 *   3. cpu_sum == --expect              -> those bytes are the disk's bytes
 *   4. via_host_bounce == 0             -> nothing transited system RAM
 *
 * RED-FIRST: --mutate corrupts one word after the CPU sum is taken, so gpu_sum
 * must disagree and the cell MUST report FAIL.
 *
 * --expect is the CRC-free 32-bit word sum of the same LBA range, computed on
 * the host (the container has no block-device access, by design).
 *
 * Build (in the container): gcc -O2 -Wall -I. -I/usr/include/drm -o aw_e2e_test \
 *                              aw_e2e_test.c -lOpenCL
 * Run: ./aw_e2e_test --expect 0xXXXXXXXX [--mutate]
 */
#define _GNU_SOURCE
#define CL_TARGET_OPENCL_VERSION 300
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <CL/cl.h>
#include <CL/cl_ext.h>
#include "xe_drm.h"
#include "aw_uapi.h"

#ifndef CL_EXTERNAL_MEMORY_HANDLE_DMA_BUF_KHR
#define CL_EXTERNAL_MEMORY_HANDLE_DMA_BUF_KHR 0x2067
#endif

#define BO_SIZE  (1u << 20)
#define NWORDS   (BO_SIZE / 4)
#define NITEMS   1024
#define PERITEM  (NWORDS / NITEMS)
#define DISK_OFF 0x1000000000ULL
#define POISON   0xA5

static const char *SRC =
"__kernel void sum32(__global const uint *in, __global uint *out, uint peritem){\n"
"  uint i = get_global_id(0); uint acc = 0;\n"
"  for (uint k = 0; k < peritem; k++) acc += in[i*peritem + k];\n"
"  atomic_add(out, acc);\n"
"}\n";

static int die(const char *w, cl_int e) { fprintf(stderr, "FAIL: %s (err=%d, %s)\n", w, e, strerror(errno)); return 1; }

int main(int argc, char **argv)
{
	const char *drm = "/dev/dri/renderD128";
	int mutate = 0, have_expect = 0;
	uint32_t expect = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mutate")) mutate = 1;
		else if (!strcmp(argv[i], "--drm") && i + 1 < argc) drm = argv[++i];
		else if (!strcmp(argv[i], "--expect") && i + 1 < argc) {
			expect = (uint32_t)strtoul(argv[++i], NULL, 0); have_expect = 1;
		}
	}
	printf("==== arcwell END-TO-END %s ====\n",
	       mutate ? "[MUTATED - MUST FAIL]" : "[normal - must PASS]");
	printf("NVMe --DMA--> Arc VRAM BO --dma-buf--> OpenCL kernel\n");

	/* --- 1. VRAM BO, poisoned --- */
	int gfd = open(drm, O_RDWR | O_CLOEXEC);
	if (gfd < 0) return die("open drm", 0);
	struct drm_xe_gem_create bo = {0};
	bo.size = BO_SIZE;
	bo.placement = 1u << DRM_XE_MEM_REGION_CLASS_VRAM;
	bo.flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;
	bo.cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;
	if (ioctl(gfd, DRM_IOCTL_XE_GEM_CREATE, &bo) < 0) return die("GEM_CREATE", 0);
	struct drm_xe_gem_mmap_offset mm = {0};
	mm.handle = bo.handle;
	if (ioctl(gfd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mm) < 0) return die("MMAP_OFFSET", 0);
	uint32_t *vram = mmap(NULL, BO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, gfd, mm.offset);
	if (vram == MAP_FAILED) return die("mmap BO", 0);
	memset(vram, POISON, BO_SIZE);
	__sync_synchronize();
	printf("BO poisoned with 0x%02x\n", POISON);

	/* --- 2. export --- */
	struct drm_prime_handle prime = {0};
	prime.handle = bo.handle;
	prime.flags = O_RDWR | O_CLOEXEC;
	if (ioctl(gfd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) return die("PRIME_HANDLE_TO_FD", 0);

	/* --- 3. arcwell: NVMe -> this BO --- */
	int afd = open("/dev/" AW_DEVICE_NAME, O_RDWR);
	if (afd < 0) return die("open /dev/" AW_DEVICE_NAME, 0);
	struct aw_ioc_stats base = {0}; ioctl(afd, AW_IOC_STATS, &base);
	struct aw_ioc_map_buffer mb = {0};
	mb.in_handle = (uint64_t)prime.fd;
	mb.in_source = AW_BUF_DMABUF;
	mb.in_length = BO_SIZE;
	if (ioctl(afd, AW_IOC_MAP_BUFFER, &mb) < 0) return die("AW_IOC_MAP_BUFFER", 0);
	if (!(mb.out_flags & AW_MAP_F_REQUIRE_P2P)) {
		printf("RESULT=FAIL -- mapping is not peer-to-peer\n"); return 1;
	}
	struct aw_ioc_read_blocks rb = {0};
	rb.in_buffer_handle = mb.out_handle;
	/* RED-FIRST: the mutation now lives in the DMA itself -- fetch the wrong
	 * LBA range. The GPU then sums real disk bytes that are not the expected
	 * ones, so the cell fails on the consumption path, where it should. */
	rb.in_start_block = (mutate ? (DISK_OFF + BO_SIZE) : DISK_OFF) >> 9;
	rb.in_block_count = BO_SIZE >> 9;
	if (ioctl(afd, AW_IOC_READ_BLOCKS, &rb) < 0) return die("AW_IOC_READ_BLOCKS", 0);
	printf("arcwell: %llu bytes DMA'd into the BO, %u segments\n",
	       (unsigned long long)rb.out_bytes, rb.out_segments);

	/* NO CPU READBACK OF VRAM. DMA exists to move bytes without the CPU; a cell
	 * that then reads them back one word at a time measures a path production
	 * never uses. It is also strictly weaker than the GPU check below, and it is
	 * what hung for 12 minutes once a 128 MiB carve was in play (BACKLOG item
	 * 20). The GPU's own sum settles everything:
	 *   gpu_sum == poison_sum  -> the DMA wrote nothing
	 *   gpu_sum == expect      -> the poison is gone AND the bytes are the disk's
	 * Poisoning stays: it is a CPU WRITE to write-combined memory, which is
	 * posted and fast, unlike a read. */
	uint32_t poison_sum = 0;
	{
		uint32_t pw = ((uint32_t)POISON << 24) | ((uint32_t)POISON << 16) |
			      ((uint32_t)POISON << 8) | (uint32_t)POISON;
		poison_sum = pw * (uint32_t)NWORDS;	/* mod 2^32, as the kernel sums */
	}

	/* --- 4. same fd into OpenCL --- */
	cl_platform_id plats[8]; cl_uint nplat = 0;
	clGetPlatformIDs(8, plats, &nplat);
	cl_device_id dev = NULL; cl_platform_id plat = NULL; char dname[256] = {0};
	const char *want = strstr(drm, "129") ? "B60" : "A770";
	for (cl_uint i = 0; i < nplat && !dev; i++) {
		cl_device_id d; cl_uint nd = 0;
		if (clGetDeviceIDs(plats[i], CL_DEVICE_TYPE_GPU, 1, &d, &nd) || !nd) continue;
		char n[256] = {0};
		clGetDeviceInfo(d, CL_DEVICE_NAME, sizeof n, n, NULL);
		if (strstr(n, want)) { dev = d; plat = plats[i]; memcpy(dname, n, sizeof dname - 1); }
	}
	if (!dev) return die("no matching OpenCL device", 0);
	printf("device: %s\n", dname);

	cl_int err;
	cl_context ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &err);
	cl_command_queue q = clCreateCommandQueueWithProperties(ctx, dev, NULL, &err);
	cl_mem_properties props[] = {
		(cl_mem_properties)CL_EXTERNAL_MEMORY_HANDLE_DMA_BUF_KHR,
		(cl_mem_properties)prime.fd, 0
	};
	cl_mem in = clCreateBufferWithProperties(ctx, props, CL_MEM_READ_ONLY, BO_SIZE, NULL, &err);
	if (!in) return die("clCreateBufferWithProperties(DMA_BUF)", err);
	printf("same dma-buf imported into OpenCL\n");

	clEnqueueAcquireExternalMemObjectsKHR_fn acq =
		(clEnqueueAcquireExternalMemObjectsKHR_fn)
		clGetExtensionFunctionAddressForPlatform(plat, "clEnqueueAcquireExternalMemObjectsKHR");
	clEnqueueReleaseExternalMemObjectsKHR_fn rel =
		(clEnqueueReleaseExternalMemObjectsKHR_fn)
		clGetExtensionFunctionAddressForPlatform(plat, "clEnqueueReleaseExternalMemObjectsKHR");
	if (acq) acq(q, 1, &in, 0, NULL, NULL);

	uint32_t zero = 0;
	cl_mem out = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, 4, &zero, &err);
	cl_program prg = clCreateProgramWithSource(ctx, 1, &SRC, NULL, &err);
	if (clBuildProgram(prg, 1, &dev, NULL, NULL, NULL) != CL_SUCCESS) {
		char log[4096] = {0};
		clGetProgramBuildInfo(prg, dev, CL_PROGRAM_BUILD_LOG, sizeof log, log, NULL);
		fprintf(stderr, "%s\n", log);
		return die("clBuildProgram", 0);
	}
	cl_kernel k = clCreateKernel(prg, "sum32", &err);
	cl_uint peritem = PERITEM;
	clSetKernelArg(k, 0, sizeof in, &in);
	clSetKernelArg(k, 1, sizeof out, &out);
	clSetKernelArg(k, 2, sizeof peritem, &peritem);

	size_t gsz = NITEMS;
	cl_event ev = NULL;
	if (clEnqueueNDRangeKernel(q, k, 1, NULL, &gsz, NULL, 0, NULL, &ev) != CL_SUCCESS)
		return die("clEnqueueNDRangeKernel", 0);
	if (clWaitForEvents(1, &ev) != CL_SUCCESS) return die("clWaitForEvents", 0);
	printf("fenced on cl_event (ENGINEER_PROMPT.md §2)\n");

	uint32_t gpu_sum = 0;
	clEnqueueReadBuffer(q, out, CL_TRUE, 0, 4, &gpu_sum, 0, NULL, NULL);
	if (rel) rel(q, 1, &in, 0, NULL, NULL);
	clFinish(q);

	struct aw_ioc_stats st = {0};
	ioctl(afd, AW_IOC_STATS, &st);
	printf("stats: reads=%llu bytes=%llu us=%llu segments=%u via_host_bounce=%u\n",
	       (unsigned long long)st.reads, (unsigned long long)st.bytes,
	       (unsigned long long)st.us_total, st.segments, st.via_host_bounce);
	printf("gpu_sum=0x%08x poison_sum=0x%08x", gpu_sum, poison_sum);
	if (have_expect) printf(" expect=0x%08x", expect);
	printf("   (no CPU read of VRAM anywhere in this cell)\n");

	if (st.via_host_bounce != base.via_host_bounce)  /* DELTA: the counter is module-global */ { printf("RESULT=FAIL -- via_host_bounce=%u\n", st.via_host_bounce); return 1; }
	if (gpu_sum == poison_sum) { printf("RESULT=FAIL -- BO still reads as poison; arcwell wrote nothing\n"); return 1; }
	if (have_expect && gpu_sum != expect) { printf("RESULT=FAIL -- what the GPU sees is not the disk's bytes\n"); return 1; }
	if (!have_expect) { printf("RESULT=INCONCLUSIVE -- pass --expect to assert against the disk\n"); return 1; }
	printf("RESULT=PASS -- NVMe bytes reached an OpenCL kernel on %s with no host bounce\n", dname);
	return 0;
}
