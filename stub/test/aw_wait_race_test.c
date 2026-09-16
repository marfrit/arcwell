// SPDX-License-Identifier: GPL-2.0
/* aw_wait_race_test — two collectors, one batch id.
 *
 * THE DEFECT THIS GUARDS. AW_IOC_BATCH_WAIT used to xa_load() the batch, wait on
 * it, then xa_erase() and free it -- with the erase's return value ignored and no
 * lock held across the sequence. Two threads waiting on the same id therefore
 * both got the same pointer, both parked on the same completion, and since the
 * completion path called complete() (which wakes exactly ONE waiter) the winner
 * freed the allocation while the loser was still parked inside it. Double
 * kfree(), double kref_put() on every buffer the batch referenced, and a loser
 * sleeping forever on freed memory.
 *
 * The two halves compound, which is why fixing one is not enough: with only the
 * erase checked, the loser still wakes on a freed completion; with only
 * complete_all(), both collectors still run the free path.
 *
 * WHAT IS ASSERTED. Exactly one thread collects. The other returns -EBUSY and
 * does NOT wait. Neither crashes, neither hangs, and a later wait on the
 * collected id returns -EINVAL.
 *
 * RUNNING THIS AGAINST THE PRE-FIX MODULE is expected to corrupt memory or hang,
 * not to print a failure. That is the nature of the bug and the reason the cell
 * exists: it is a regression guard, and its green is only meaningful because the
 * red legs below show the assertions can discriminate.
 *
 * RED-FIRST, two independent legs:
 *   --mutate    asserts the BUGGY contract (both collectors succeed). Must FAIL.
 *   --serial    collects twice in sequence, no race. The second must be -EINVAL;
 *               if it succeeds, the id outlived its collection and the cell fails.
 *
 * Build: gcc -O2 -Wall -I../include -I. -I/usr/include/drm -o aw_wait_race_test \
 *            aw_wait_race_test.c -lpthread
 * Run:   ./aw_wait_race_test [--mutate] [--serial] [--drm PATH]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include "xe_drm.h"
#include "aw_uapi.h"

#define NBUF      16
#define BO_SIZE   (1u << 20)
#define DISK_OFF  0x1000000000ULL

static int afd;
static uint64_t batch_id;
static pthread_barrier_t gate;

struct leg {
	int rc;			/* 0, or -errno */
	uint64_t bytes;
	uint32_t completed;
};

static int fail(const char *what) { printf("RESULT=FAIL -- %s (%s)\n", what, strerror(errno)); return 1; }

/* Both threads enter the wait as close together as the barrier allows, so the
 * loser is parked before the winner can collect. That ordering is the whole
 * point: a serialised pair does not reproduce the defect. */
static void *collector(void *arg)
{
	struct leg *out = arg;
	struct aw_ioc_batch_wait bw;

	memset(&bw, 0, sizeof bw);
	bw.in_batch_id = batch_id;
	bw.in_timeout_us = UINT64_MAX;		/* block: the dangerous case */

	pthread_barrier_wait(&gate);
	out->rc = ioctl(afd, AW_IOC_BATCH_WAIT, &bw) < 0 ? -errno : 0;
	out->bytes = bw.out_bytes;
	out->completed = bw.out_completed;
	return NULL;
}

int main(int argc, char **argv)
{
	const char *drm = "/dev/dri/renderD128";
	int mutate = 0, serial = 0, i;
	uint32_t handles[NBUF];
	struct aw_ioc_read_blocks reqs[NBUF];
	struct aw_ioc_batch_submit sb;
	struct leg a = {0}, b = {0};
	pthread_t ta, tb;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mutate")) mutate = 1;
		else if (!strcmp(argv[i], "--serial")) serial = 1;
		else if (!strcmp(argv[i], "--drm") && i + 1 < argc) drm = argv[++i];
	}
	printf("==== two collectors, one batch id %s ====\n",
	       mutate ? "[MUTATED: asserts the BUGGY contract - MUST FAIL]"
		      : serial ? "[serial: second collect must be -EINVAL]" : "[normal]");

	int gfd = open(drm, O_RDWR | O_CLOEXEC);
	if (gfd < 0) return fail("open drm");
	afd = open("/dev/" AW_DEVICE_NAME, O_RDWR);
	if (afd < 0) return fail("open /dev/" AW_DEVICE_NAME);

	for (i = 0; i < NBUF; i++) {
		struct drm_xe_gem_create bo;
		struct drm_prime_handle pr;
		struct aw_ioc_map_buffer mb;

		memset(&bo, 0, sizeof bo);
		bo.size = BO_SIZE;
		bo.placement = 1u << DRM_XE_MEM_REGION_CLASS_VRAM;
		bo.flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;
		bo.cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;
		if (ioctl(gfd, DRM_IOCTL_XE_GEM_CREATE, &bo) < 0) return fail("GEM_CREATE");

		memset(&pr, 0, sizeof pr);
		pr.handle = bo.handle; pr.flags = O_RDWR | O_CLOEXEC;
		if (ioctl(gfd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &pr) < 0) return fail("PRIME");

		memset(&mb, 0, sizeof mb);
		mb.in_handle = (uint64_t)pr.fd;
		mb.in_source = AW_BUF_DMABUF;
		mb.in_length = BO_SIZE;
		if (ioctl(afd, AW_IOC_MAP_BUFFER, &mb) < 0) return fail("MAP_BUFFER");
		if (!(mb.out_flags & AW_MAP_F_REQUIRE_P2P)) {
			printf("RESULT=FAIL -- mapping is not peer-to-peer\n"); return 1;
		}
		handles[i] = mb.out_handle;
		close(pr.fd);

		memset(&reqs[i], 0, sizeof reqs[i]);
		reqs[i].in_buffer_handle = handles[i];
		reqs[i].in_start_block = (DISK_OFF + (uint64_t)i * BO_SIZE) >> 9;
		reqs[i].in_block_count = BO_SIZE >> 9;
	}

	memset(&sb, 0, sizeof sb);
	sb.in_requests = (uint64_t)(uintptr_t)reqs;
	sb.in_count = NBUF;
	if (ioctl(afd, AW_IOC_SUBMIT_BATCH, &sb) < 0) return fail("AW_IOC_SUBMIT_BATCH");
	batch_id = sb.out_batch_id;
	printf("submitted batch id=%llu, %d requests\n", (unsigned long long)batch_id, NBUF);

	if (serial) {
		struct aw_ioc_batch_wait w1, w2;
		int r1, r2;

		memset(&w1, 0, sizeof w1); w1.in_batch_id = batch_id; w1.in_timeout_us = UINT64_MAX;
		r1 = ioctl(afd, AW_IOC_BATCH_WAIT, &w1) < 0 ? -errno : 0;
		memset(&w2, 0, sizeof w2); w2.in_batch_id = batch_id; w2.in_timeout_us = UINT64_MAX;
		r2 = ioctl(afd, AW_IOC_BATCH_WAIT, &w2) < 0 ? -errno : 0;
		printf("  first collect rc=%d bytes=%llu ; second collect rc=%d (%s)\n",
		       r1, (unsigned long long)w1.out_bytes, r2, r2 ? strerror(-r2) : "SUCCEEDED");
		if (r1 != 0) { printf("RESULT=FAIL -- first collect failed\n"); return 1; }
		if (r2 != -EINVAL) {
			printf("RESULT=FAIL -- a collected id must be -EINVAL, got %d\n", r2);
			return 1;
		}
		printf("RESULT=PASS -- the id dies with its collection\n");
		close(afd); close(gfd);
		return 0;
	}

	pthread_barrier_init(&gate, NULL, 2);
	pthread_create(&ta, NULL, collector, &a);
	pthread_create(&tb, NULL, collector, &b);
	pthread_join(ta, NULL);
	pthread_join(tb, NULL);
	pthread_barrier_destroy(&gate);

	printf("  A: rc=%-8d bytes=%llu completed=%u\n", a.rc, (unsigned long long)a.bytes, a.completed);
	printf("  B: rc=%-8d bytes=%llu completed=%u\n", b.rc, (unsigned long long)b.bytes, b.completed);

	int wins = (a.rc == 0) + (b.rc == 0);
	int busy = (a.rc == -EBUSY) + (b.rc == -EBUSY);

	if (mutate) {
		/* Assert the defect's contract: both collectors succeed. On a fixed
		 * module exactly one does, so this leg must go red. */
		if (wins == 2) { printf("RESULT=PASS -- both collected (this is the BUG)\n"); return 0; }
		printf("RESULT=FAIL -- only %d collector(s) succeeded; the buggy contract does not hold, which is correct\n", wins);
		return 1;
	}

	if (wins != 1) {
		printf("RESULT=FAIL -- %d collectors succeeded, must be exactly 1\n", wins);
		return 1;
	}
	if (busy != 1) {
		printf("RESULT=FAIL -- the loser must get -EBUSY, got %d\n",
		       a.rc == 0 ? b.rc : a.rc);
		return 1;
	}

	/* The winner must have collected real work, not an empty husk. */
	uint64_t got = a.rc == 0 ? a.bytes : b.bytes;
	if (got != (uint64_t)NBUF * BO_SIZE) {
		printf("RESULT=FAIL -- winner collected %llu bytes, expected %llu\n",
		       (unsigned long long)got, (unsigned long long)NBUF * BO_SIZE);
		return 1;
	}

	/* And the id must now be dead for everyone. */
	struct aw_ioc_batch_wait after;
	memset(&after, 0, sizeof after);
	after.in_batch_id = batch_id;
	after.in_timeout_us = 0;
	int rc_after = ioctl(afd, AW_IOC_BATCH_WAIT, &after) < 0 ? -errno : 0;
	if (rc_after != -EINVAL) {
		printf("RESULT=FAIL -- collected id still answers (%d), must be -EINVAL\n", rc_after);
		return 1;
	}

	struct aw_ioc_stats st;
	memset(&st, 0, sizeof st);
	ioctl(afd, AW_IOC_STATS, &st);
	printf("  one collector took %llu bytes, the other got -EBUSY without waiting; "
	       "id now -EINVAL; via_host_bounce=%u\n", (unsigned long long)got, st.via_host_bounce);
	printf("RESULT=PASS -- exactly one collector, no double free, no parked loser\n");
	close(afd); close(gfd);
	return 0;
}
