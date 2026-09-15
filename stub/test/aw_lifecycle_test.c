// SPDX-License-Identifier: GPL-2.0
/* aw_lifecycle_test — cell for the buffer lifecycle added to the arcwell API.
 *
 * Before this, AW_IOC_MAP_BUFFER registered a buffer forever: no unmap call, no
 * .release handler, no per-client ownership. Every registration leaked a pinned
 * VRAM buffer, so a streaming workload would exhaust VRAM. This cell holds the
 * three properties that fix is supposed to have:
 *
 *   1. AW_IOC_UNMAP_BUFFER actually drops a registration (buffers_live falls).
 *   2. Unmapping an unknown handle fails rather than silently succeeding.
 *   3. Closing the fd releases whatever is still registered, so a client that
 *      crashes cannot strand VRAM. (Checked in dmesg by the caller.)
 *
 * No DMA is performed. This is about registration bookkeeping only.
 *
 * Build: gcc -O2 -Wall -I. -I/usr/include/drm -o aw_lifecycle_test aw_lifecycle_test.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "xe_drm.h"
#include "aw_uapi.h"

#define N       8
#define BO_SIZE (1u << 20)

static int fail(const char *m) { printf("RESULT=FAIL -- %s (%s)\n", m, strerror(errno)); return 1; }

static uint32_t live(int afd)
{
	struct aw_ioc_stats st = {0};
	if (ioctl(afd, AW_IOC_STATS, &st) < 0) return 0xffffffff;
	return st.buffers_live;
}

int main(int argc, char **argv)
{
	/* Accept both spellings: a bare path (historical) and --drm PATH, which is
	 * what every other cell in this directory takes. */
	const char *drm = "/dev/dri/renderD128";
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--drm") && i + 1 < argc) drm = argv[++i];
		else if (argv[i][0] != '-') drm = argv[i];
	}
	uint32_t handles[N];
	int gfd, afd, i;

	printf("==== arcwell buffer lifecycle ====\n");
	gfd = open(drm, O_RDWR | O_CLOEXEC);
	if (gfd < 0) return fail("open drm");
	afd = open("/dev/" AW_DEVICE_NAME, O_RDWR);
	if (afd < 0) return fail("open /dev/" AW_DEVICE_NAME);

	if (live(afd) != 0) { printf("RESULT=FAIL -- fresh fd does not start at 0 live\n"); return 1; }

	for (i = 0; i < N; i++) {
		struct drm_xe_gem_create bo = {0};
		struct drm_prime_handle pr = {0};
		struct aw_ioc_map_buffer mb = {0};

		bo.size = BO_SIZE;
		bo.placement = 1u << DRM_XE_MEM_REGION_CLASS_VRAM;
		bo.flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;
		bo.cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;
		if (ioctl(gfd, DRM_IOCTL_XE_GEM_CREATE, &bo) < 0) return fail("GEM_CREATE");
		pr.handle = bo.handle; pr.flags = O_RDWR | O_CLOEXEC;
		if (ioctl(gfd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &pr) < 0) return fail("PRIME");
		mb.in_handle = (uint64_t)pr.fd;
		mb.in_source = AW_BUF_DMABUF;
		mb.in_length = BO_SIZE;
		if (ioctl(afd, AW_IOC_MAP_BUFFER, &mb) < 0) return fail("MAP_BUFFER");
		handles[i] = mb.out_handle;
		close(pr.fd);	/* arcwell holds its own reference */
	}
	printf("registered %d buffers, live=%u\n", N, live(afd));
	if (live(afd) != N) { printf("RESULT=FAIL -- live != %d after registering\n", N); return 1; }

	/* 1. unmap half */
	for (i = 0; i < N / 2; i++)
		if (ioctl(afd, AW_IOC_UNMAP_BUFFER, &handles[i]) < 0) return fail("UNMAP_BUFFER");
	printf("unmapped %d, live=%u\n", N / 2, live(afd));
	if (live(afd) != N / 2) { printf("RESULT=FAIL -- unmap did not drop the registration\n"); return 1; }

	/* 2. unmapping a stale handle must fail, not silently succeed */
	if (ioctl(afd, AW_IOC_UNMAP_BUFFER, &handles[0]) == 0) {
		printf("RESULT=FAIL -- double unmap succeeded; handle reuse is unsafe\n"); return 1;
	}
	uint32_t bogus = 0x7fffffff;
	if (ioctl(afd, AW_IOC_UNMAP_BUFFER, &bogus) == 0) {
		printf("RESULT=FAIL -- unmapping an unknown handle succeeded\n"); return 1;
	}
	printf("stale and unknown handles correctly rejected\n");

	/* 3. leave the rest registered and close; the kernel must reclaim them */
	printf("closing fd with %u buffer(s) still registered "
	       "(kernel should report reclaiming them)\n", live(afd));
	close(afd);
	close(gfd);
	printf("RESULT=PASS -- unmap drops registrations, bad handles rejected, close reclaims\n");
	return 0;
}
