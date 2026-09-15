// SPDX-License-Identifier: GPL-2.0
/* aw_bfabric — B_FABRIC: what PCIe can actually deliver host->device on this card.
 *
 * Entry criterion of docs/campaigns/miss-tier-direct.md. It bounds B_P_host and
 * B_P_arcwell and needs neither arcint nor arcwell.
 *
 * Method follows design-qwen-flash-next.md:1315 -- "a repeated non-blocking
 * upload of a known expert-slot-sized buffer ... amortised ... batched, not
 * per-tensor". Source is pinned (CL_MEM_ALLOC_HOST_PTR) host memory; destination
 * is a device-resident buffer; N non-blocking writes are queued, then one
 * clFinish, and the whole batch is amortised. Best and median of R rounds.
 *
 * THE CONTROL, which is what makes this able to fail: the same batch is run at
 * 4 KiB per transfer. A 4 KiB batch is latency-bound and MUST come out far slower
 * in GB/s than the slot-sized one. If the two rates are within 2x, the harness is
 * measuring queue bookkeeping rather than bytes on the wire, and the run is void.
 *
 * Reports nothing about arcwell. This is the ceiling both paths live under.
 *
 * Build: gcc -O2 -Wall -I. -I/usr/include/drm -o aw_bfabric aw_bfabric.c -lOpenCL
 */
#define _GNU_SOURCE
#define CL_TARGET_OPENCL_VERSION 300
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <CL/cl.h>

#define SLOT   2457600u   /* per-expert slice, design-qwen-flash-next.md:1025 */
#define SMALL  4096u      /* control */
#define NBATCH 64
#define ROUNDS 12

static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static int cmpd(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return x<y?-1:x>y;}

/* returns GB/s */
static double run_batch(cl_context ctx, cl_command_queue q, size_t xfer,
                        const char *label)
{
    cl_int err;
    size_t total = xfer * NBATCH;
    cl_mem dev = clCreateBuffer(ctx, CL_MEM_READ_WRITE, total, NULL, &err);
    cl_mem pin = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR, xfer, NULL, &err);
    if (!dev || !pin) { fprintf(stderr, "alloc failed (%d)\n", err); return -1; }
    void *host = clEnqueueMapBuffer(q, pin, CL_TRUE, CL_MAP_WRITE, 0, xfer, 0, NULL, NULL, &err);
    if (!host) { fprintf(stderr, "map failed (%d)\n", err); return -1; }
    memset(host, 0x5a, xfer);
    clEnqueueUnmapMemObject(q, pin, host, 0, NULL, NULL);
    clFinish(q);
    host = clEnqueueMapBuffer(q, pin, CL_TRUE, CL_MAP_READ, 0, xfer, 0, NULL, NULL, &err);

    double best = 0, all[ROUNDS];
    for (int r = 0; r < ROUNDS; r++) {
        double t0 = now();
        for (unsigned i = 0; i < NBATCH; i++)
            clEnqueueWriteBuffer(q, dev, CL_FALSE, (size_t)i * xfer, xfer, host, 0, NULL, NULL);
        clFinish(q);                       /* one fence for the whole batch */
        double dt = now() - t0;
        double gbs = (double)total / dt / 1e9;
        all[r] = gbs;
        if (gbs > best) best = gbs;
    }
    qsort(all, ROUNDS, sizeof(double), cmpd);
    double med = all[ROUNDS/2];
    printf("  %-18s xfer=%-9zu batch=%d  best=%6.2f GB/s  median=%6.2f GB/s\n",
           label, xfer, NBATCH, best, med);
    clEnqueueUnmapMemObject(q, pin, host, 0, NULL, NULL);
    clFinish(q);
    clReleaseMemObject(pin); clReleaseMemObject(dev);
    return med;
}

int main(int argc, char **argv)
{
    const char *want = argc > 1 ? argv[1] : "B60";
    cl_platform_id plats[8]; cl_uint np = 0;
    clGetPlatformIDs(8, plats, &np);
    cl_device_id dev = NULL; char name[256] = {0};
    for (cl_uint i = 0; i < np && !dev; i++) {
        cl_device_id d; cl_uint nd = 0;
        if (clGetDeviceIDs(plats[i], CL_DEVICE_TYPE_GPU, 1, &d, &nd) || !nd) continue;
        char n[256] = {0};
        clGetDeviceInfo(d, CL_DEVICE_NAME, sizeof n, n, NULL);
        if (strstr(n, want)) { dev = d; memcpy(name, n, sizeof name - 1); }
    }
    if (!dev) { printf("RESULT=FAIL -- no OpenCL device matching '%s'\n", want); return 1; }

    cl_int err;
    cl_context ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &err);
    cl_command_queue q = clCreateCommandQueueWithProperties(ctx, dev, NULL, &err);
    printf("==== B_FABRIC: host->device ceiling ====\n");
    printf("device: %s\n", name);

    double slot  = run_batch(ctx, q, SLOT,  "slot-sized");
    double small = run_batch(ctx, q, SMALL, "4KiB control");
    if (slot < 0 || small < 0) { printf("RESULT=FAIL -- a batch did not run\n"); return 1; }

    printf("\ncontrol ratio slot/4KiB = %.1fx\n", slot / small);
    if (slot / small < 2.0) {
        printf("RESULT=VOID -- the 4 KiB control is within 2x of the slot-sized rate, so\n"
               "this harness is timing queue bookkeeping, not bytes on the wire.\n");
        return 1;
    }
    printf("RESULT=OK -- B_FABRIC (median, slot-sized, batched) = %.2f GB/s on %s\n", slot, name);
    return 0;
}
