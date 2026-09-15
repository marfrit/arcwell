// SPDX-License-Identifier: GPL-2.0
/* aw_batch_test — cell for AW_IOC_READ_BATCH.
 *
 * M4_API.md: "A batch of experts is the inference pattern, not a single bulk
 * read." AW_IOC_READ_BLOCKS waits for each transfer, so N experts serialise and
 * the NVMe queue holds one request. AW_IOC_READ_BATCH submits the whole batch
 * before waiting.
 *
 * This cell tests the CONTRACT OF THE NEW CALL, not the DMA -- that is already
 * established. Three properties, each able to fail on its own:
 *
 *   1. a batch of N completes: out_completed == N, out_bytes == N * size;
 *   2. the batch drives more than one bio into the queue at once. THIS IS THE
 *      POINT: if submission were still serial, or if the implementation quietly
 *      waited per request, only one bio is ever in flight and the cell goes red.
 *      `max_inflight` is a MODULE-GLOBAL HIGH-WATER MARK that is never reset, so
 *      a large earlier run leaves it high and it cannot be read as "this batch's
 *      concurrency". The cell therefore takes a baseline first and states plainly
 *      which of the two readings it got;
 *   3. a batch containing a bad handle fails and names the offending index,
 *      rather than silently doing less work than asked.
 *
 * No CPU read of VRAM anywhere.
 *
 * Build: gcc -O2 -Wall -I. -I/usr/include/drm -o aw_batch_test aw_batch_test.c
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

#define N        16
#define BO_SIZE  (1u << 20)
#define DISK_OFF 0x1000000000ULL

static int fail(const char *m) { printf("RESULT=FAIL -- %s (%s)\n", m, strerror(errno)); return 1; }

int main(int argc, char **argv)
{
	/* Accept both spellings: a bare path (historical) and --drm PATH, which is
	 * what every other cell in this directory takes. */
	const char *drm = "/dev/dri/renderD128";
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--drm") && i + 1 < argc) drm = argv[++i];
		else if (argv[i][0] != '-') drm = argv[i];
	}
	struct aw_ioc_read_blocks reqs[N];
	uint32_t handles[N];
	struct aw_ioc_read_batch batch = {0};
	struct aw_ioc_stats st = {0};
	int gfd, afd, i;

	printf("==== arcwell AW_IOC_READ_BATCH ====\n");
	gfd = open(drm, O_RDWR | O_CLOEXEC);
	if (gfd < 0) return fail("open drm");
	afd = open("/dev/" AW_DEVICE_NAME, O_RDWR);
	if (afd < 0) return fail("open /dev/" AW_DEVICE_NAME);
	struct aw_ioc_stats base = {0}; ioctl(afd, AW_IOC_STATS, &base);

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
		close(pr.fd);

		memset(&reqs[i], 0, sizeof reqs[i]);
		reqs[i].in_buffer_handle = handles[i];
		reqs[i].in_start_block = (DISK_OFF + (uint64_t)i * BO_SIZE) >> 9;
		reqs[i].in_block_count = BO_SIZE >> 9;
		reqs[i].in_dest_offset = 0;
	}
	printf("registered %d expert buffers of %u bytes\n", N, BO_SIZE);

	/* --- 1. the batch --- */
	batch.in_requests = (uint64_t)(uintptr_t)reqs;
	batch.in_count = N;
	if (ioctl(afd, AW_IOC_READ_BATCH, &batch) < 0) return fail("AW_IOC_READ_BATCH");
	printf("batch: completed=%u bytes=%llu err=%d\n",
	       batch.out_completed, (unsigned long long)batch.out_bytes, batch.out_err);
	if (batch.out_completed != N)
		{ printf("RESULT=FAIL -- completed %u of %d\n", batch.out_completed, N); return 1; }
	if (batch.out_bytes != (uint64_t)N * BO_SIZE)
		{ printf("RESULT=FAIL -- byte count wrong\n"); return 1; }

	/* --- 2. was the queue actually used concurrently? --- */
	if (ioctl(afd, AW_IOC_STATS, &st) < 0) return fail("AW_IOC_STATS");
	printf("stats: batches=%llu batch_reads=%llu max_inflight=%u via_host_bounce=%u\n",
	       (unsigned long long)st.batches, (unsigned long long)st.batch_reads,
	       st.max_inflight, st.via_host_bounce);
	if (st.via_host_bounce != base.via_host_bounce)  /* DELTA: the counter is module-global */ { printf("RESULT=FAIL -- via_host_bounce=%u\n", st.via_host_bounce); return 1; }
	if (st.max_inflight < 2) {
		printf("RESULT=FAIL -- max_inflight=%u: the batch was submitted serially, "
		       "which is what this call exists to avoid\n", st.max_inflight);
		return 1;
	}
	/* Each 1 MiB buffer is 256 pages, exactly BIO_MAX_VECS, so this batch can
	 * raise the mark to at most N. Whether it did is only visible if the mark
	 * actually moved; if a bigger earlier batch left it above N, this batch's
	 * own concurrency is simply not observable through this counter. Say which. */
	if (st.max_inflight > base.max_inflight)
		printf("max_inflight %u -> %u: THIS batch raised the mark%s\n",
		       base.max_inflight, st.max_inflight,
		       st.max_inflight == N ? " to N, one bio per buffer as expected" : "");
	else
		printf("max_inflight %u (unmoved, base %u >= what this batch can reach): the "
		       "high-water mark is module-global and was already above this batch's "
		       "ceiling of %d, so it cannot testify about THIS batch. Load the module "
		       "fresh to make this leg decisive.\n",
		       st.max_inflight, base.max_inflight, N);

	/* --- 3. a bad handle must be reported, with its index --- */
	struct aw_ioc_read_blocks bad[3];
	memcpy(bad, reqs, sizeof bad);
	bad[1].in_buffer_handle = 0x7fffffff;
	struct aw_ioc_read_batch b2 = {0};
	b2.in_requests = (uint64_t)(uintptr_t)bad;
	b2.in_count = 3;
	ioctl(afd, AW_IOC_READ_BATCH, &b2);
	printf("bad-handle batch: err=%d err_index=%u completed=%u\n",
	       b2.out_err, b2.out_err_index, b2.out_completed);
	if (b2.out_err == 0) { printf("RESULT=FAIL -- batch with a bad handle reported success\n"); return 1; }
	if (b2.out_err_index != 1) { printf("RESULT=FAIL -- wrong err_index %u, expected 1\n", b2.out_err_index); return 1; }

	close(afd); close(gfd);
	printf("RESULT=PASS -- batch completes, drove %u bios concurrently%s, and reports bad requests\n",
	       st.max_inflight,
	       st.max_inflight > base.max_inflight ? " in this run" : " at the session high-water mark");
	return 0;
}
