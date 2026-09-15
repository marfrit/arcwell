// SPDX-License-Identifier: GPL-2.0
/* aw_cl_import_test — BACKLOG item 15, part 1 of 2.
 *
 * Isolates the CONSUMPTION half of the chain from the DMA half. arcwell is not
 * involved. The question here is only:
 *
 *   can an OpenCL kernel on the Arc read a VRAM BO that was exported as a
 *   dma-buf and imported via cl_khr_external_memory_dma_buf?
 *
 * If yes, the only thing standing between arcwell and a proven consumption path
 * is wiring /dev/arcwell into the container -- a permissions question, not an
 * engineering one. ENGINEER_PROMPT.md §2 requires the OpenCL/OpenVINO path be
 * confirmed and completion fenced on an OpenCL event, not Level Zero. This does
 * both: it waits on a cl_event from the kernel launch.
 *
 * RED-FIRST: --mutate corrupts one word of the buffer after the CPU reference
 * sum is taken, so the GPU sum must disagree and the cell MUST report FAIL.
 *
 * Build: gcc -O2 -Wall -I<xe_drm.h dir> -o aw_cl_import_test aw_cl_import_test.c -lOpenCL
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

#ifndef CL_EXTERNAL_MEMORY_HANDLE_DMA_BUF_KHR
#define CL_EXTERNAL_MEMORY_HANDLE_DMA_BUF_KHR 0x2067
#endif

#define BO_SIZE (1u << 20)
#define NWORDS  (BO_SIZE / 4)
#define NITEMS  1024
#define PERITEM (NWORDS / NITEMS)

static const char *SRC =
"__kernel void sum32(__global const uint *in, __global uint *out, uint peritem){\n"
"  uint i = get_global_id(0); uint acc = 0;\n"
"  for (uint k = 0; k < peritem; k++) acc += in[i*peritem + k];\n"
"  atomic_add(out, acc);\n"
"}\n";

static int die(const char *w, cl_int e) { fprintf(stderr, "FAIL: %s (err=%d, errno=%s)\n", w, e, strerror(errno)); return 1; }

int main(int argc, char **argv)
{
	const char *drm = "/dev/dri/renderD128";
	int mutate = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mutate")) mutate = 1;
		else if (!strcmp(argv[i], "--drm") && i + 1 < argc) drm = argv[++i];
	}
	printf("==== OpenCL dma-buf consumption %s ====\n",
	       mutate ? "[MUTATED - MUST FAIL]" : "[normal - must PASS]");

	/* --- 1. VRAM BO on xe --- */
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

	/* Stand-in for what arcwell would DMA in: a deterministic pattern. */
	for (unsigned i = 0; i < NWORDS; i++) vram[i] = i * 2654435761u + 12345u;
	__sync_synchronize();

	uint32_t cpu_sum = 0;
	for (unsigned i = 0; i < NWORDS; i++) cpu_sum += vram[i];
	printf("BO filled, CPU reference sum = 0x%08x\n", cpu_sum);

	if (mutate) { vram[NWORDS / 2] ^= 0xdeadbeefu; __sync_synchronize();
		      printf("MUTATED one word after taking the reference sum\n"); }

	/* --- 2. export as dma-buf --- */
	struct drm_prime_handle prime = {0};
	prime.handle = bo.handle;
	prime.flags = O_RDWR | O_CLOEXEC;
	if (ioctl(gfd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) return die("PRIME_HANDLE_TO_FD", 0);
	printf("dma-buf fd = %d\n", prime.fd);

	/* --- 3. pick the device that matches this render node --- */
	cl_platform_id plats[8]; cl_uint nplat = 0;
	clGetPlatformIDs(8, plats, &nplat);
	cl_device_id dev = NULL; cl_platform_id plat = NULL; char dname[256] = {0};
	const char *want = strstr(drm, "129") ? "B60" : "A770";
	for (cl_uint i = 0; i < nplat && !dev; i++) {
		cl_device_id d; cl_uint nd = 0;
		if (clGetDeviceIDs(plats[i], CL_DEVICE_TYPE_GPU, 1, &d, &nd) || !nd) continue;
		char n[256] = {0};
		clGetDeviceInfo(d, CL_DEVICE_NAME, sizeof n, n, NULL);
		if (strstr(n, want)) { dev = d; plat = plats[i]; strncpy(dname, n, sizeof dname - 1); }
	}
	if (!dev) return die("no matching OpenCL device", 0);
	printf("device: %s\n", dname);

	cl_int err;
	cl_context ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &err);
	if (!ctx) return die("clCreateContext", err);
	cl_command_queue q = clCreateCommandQueueWithProperties(ctx, dev, NULL, &err);
	if (!q) return die("clCreateCommandQueue", err);

	/* --- 4. THE IMPORT --- */
	cl_mem_properties props[] = {
		(cl_mem_properties)CL_EXTERNAL_MEMORY_HANDLE_DMA_BUF_KHR,
		(cl_mem_properties)prime.fd, 0
	};
	cl_mem in = clCreateBufferWithProperties(ctx, props, CL_MEM_READ_ONLY, BO_SIZE, NULL, &err);
	if (!in) return die("clCreateBufferWithProperties(DMA_BUF import)", err);
	printf("dma-buf imported into OpenCL\n");

	/* Acquire, if the runtime exposes the entry point. */
	clEnqueueAcquireExternalMemObjectsKHR_fn acq =
		(clEnqueueAcquireExternalMemObjectsKHR_fn)
		clGetExtensionFunctionAddressForPlatform(plat, "clEnqueueAcquireExternalMemObjectsKHR");
	clEnqueueReleaseExternalMemObjectsKHR_fn rel =
		(clEnqueueReleaseExternalMemObjectsKHR_fn)
		clGetExtensionFunctionAddressForPlatform(plat, "clEnqueueReleaseExternalMemObjectsKHR");
	printf("acquire/release entry points: %s\n", acq && rel ? "present" : "absent (skipping)");
	if (acq) acq(q, 1, &in, 0, NULL, NULL);

	/* --- 5. GPU kernel reads the BO --- */
	uint32_t zero = 0;
	cl_mem out = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, 4, &zero, &err);
	if (!out) return die("out buffer", err);

	cl_program prg = clCreateProgramWithSource(ctx, 1, &SRC, NULL, &err);
	if (clBuildProgram(prg, 1, &dev, NULL, NULL, NULL) != CL_SUCCESS) {
		char log[4096] = {0};
		clGetProgramBuildInfo(prg, dev, CL_PROGRAM_BUILD_LOG, sizeof log, log, NULL);
		fprintf(stderr, "build log:\n%s\n", log);
		return die("clBuildProgram", 0);
	}
	cl_kernel k = clCreateKernel(prg, "sum32", &err);
	cl_uint peritem = PERITEM;
	clSetKernelArg(k, 0, sizeof in, &in);
	clSetKernelArg(k, 1, sizeof out, &out);
	clSetKernelArg(k, 2, sizeof peritem, &peritem);

	size_t gsz = NITEMS;
	cl_event ev = NULL;
	err = clEnqueueNDRangeKernel(q, k, 1, NULL, &gsz, NULL, 0, NULL, &ev);
	if (err != CL_SUCCESS) return die("clEnqueueNDRangeKernel", err);

	/* ENGINEER_PROMPT.md §2: fence on an OpenCL event, not Level Zero. */
	err = clWaitForEvents(1, &ev);
	if (err != CL_SUCCESS) return die("clWaitForEvents", err);
	printf("fenced on cl_event from the kernel launch\n");

	uint32_t gpu_sum = 0;
	clEnqueueReadBuffer(q, out, CL_TRUE, 0, 4, &gpu_sum, 0, NULL, NULL);
	if (rel) rel(q, 1, &in, 0, NULL, NULL);
	clFinish(q);

	printf("cpu_sum=0x%08x gpu_sum=0x%08x\n", cpu_sum, gpu_sum);
	if (gpu_sum != cpu_sum) { printf("RESULT=FAIL -- GPU read does not match buffer contents\n"); return 1; }
	printf("RESULT=PASS -- an OpenCL kernel on %s read the xe VRAM BO through a dma-buf import\n", dname);
	return 0;
}
