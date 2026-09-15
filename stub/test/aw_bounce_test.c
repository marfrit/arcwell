// SPDX-License-Identifier: GPL-2.0
/* aw_bounce_test — proves the host-bounce detector is real, not decoration.
 *
 * Five cells in this tree assert via_host_bounce == 0. Until now that field had
 * NO increment site anywhere in the module: it was zero by construction, so every
 * one of those assertions was unfalsifiable — decoration under HOUSE_RULES.md §5.
 * A reviewer caught it; this cell is the fix's guard.
 *
 * Two arms, and they must disagree:
 *   normal  : AW_IOC_MAP_BUFFER succeeds, REQUIRE_P2P set, via_host_bounce == 0
 *   forced  : module loaded with debug_force_bounce=1 attaches WITHOUT peer2peer
 *             (defect D3 on purpose), so xe migrates the BO to system RAM. The
 *             module must DETECT it, increment via_host_bounce, and REFUSE.
 *
 * If the forced arm succeeds, or leaves the counter at 0, the detector does not
 * work and every via_host_bounce assertion in this tree is worthless.
 *
 * Build: gcc -O2 -Wall -I. -I/usr/include/drm -o aw_bounce_test aw_bounce_test.c
 * Run:   aw_bounce_test [--expect-bounce]
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

#define BO_SIZE (1u << 20)

int main(int argc, char **argv)
{
	const char *drm = "/dev/dri/renderD129";
	int expect_bounce = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--expect-bounce")) expect_bounce = 1;
		else if (!strcmp(argv[i], "--drm") && i+1 < argc) drm = argv[++i];
	}
	printf("==== host-bounce detector %s ====\n",
	       expect_bounce ? "[forced: must DETECT and REFUSE]" : "[normal: must map cleanly]");

	int gfd = open(drm, O_RDWR|O_CLOEXEC); if (gfd < 0) { perror("drm"); return 1; }
	struct drm_xe_gem_create bo = {0};
	bo.size = BO_SIZE;
	bo.placement = 1u << DRM_XE_MEM_REGION_CLASS_VRAM;
	bo.flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;
	bo.cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;
	if (ioctl(gfd, DRM_IOCTL_XE_GEM_CREATE, &bo) < 0) { perror("GEM_CREATE"); return 1; }
	struct drm_prime_handle pr = {0}; pr.handle = bo.handle; pr.flags = O_RDWR|O_CLOEXEC;
	if (ioctl(gfd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &pr) < 0) { perror("PRIME"); return 1; }

	int afd = open("/dev/" AW_DEVICE_NAME, O_RDWR); if (afd < 0) { perror("arcwell"); return 1; }
	struct aw_ioc_stats before = {0};
	ioctl(afd, AW_IOC_STATS, &before);

	struct aw_ioc_map_buffer mb = {0};
	mb.in_handle = (uint64_t)pr.fd; mb.in_source = AW_BUF_DMABUF; mb.in_length = BO_SIZE;
	int rc = ioctl(afd, AW_IOC_MAP_BUFFER, &mb);
	int e = errno;

	struct aw_ioc_stats after = {0};
	ioctl(afd, AW_IOC_STATS, &after);
	unsigned delta = after.via_host_bounce - before.via_host_bounce;

	printf("  MAP_BUFFER rc=%d%s%s ; via_host_bounce %u -> %u (delta %u)\n",
	       rc, rc < 0 ? " errno=" : "", rc < 0 ? strerror(e) : "",
	       before.via_host_bounce, after.via_host_bounce, delta);

	if (expect_bounce) {
		if (rc == 0) {
			printf("RESULT=FAIL -- the mapping SUCCEEDED with peer2peer disabled.\n"
			       "That is a silent host bounce: the exact failure via_host_bounce exists to catch.\n");
			return 1;
		}
		if (delta == 0) {
			printf("RESULT=FAIL -- refused (good) but via_host_bounce did not move,\n"
			       "so the counter still proves nothing and every assertion on it is decoration.\n");
			return 1;
		}
		printf("RESULT=PASS -- detected, counted (+%u) and refused with %s\n", delta, strerror(e));
		return 0;
	}

	if (rc < 0) { printf("RESULT=FAIL -- normal mapping refused: %s\n", strerror(e)); return 1; }
	if (!(mb.out_flags & AW_MAP_F_REQUIRE_P2P)) { printf("RESULT=FAIL -- REQUIRE_P2P not honoured\n"); return 1; }
	if (delta != 0) { printf("RESULT=FAIL -- counter moved on a clean mapping\n"); return 1; }
	printf("RESULT=PASS -- mapped peer-to-peer, counter unmoved\n");
	return 0;
}
