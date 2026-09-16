// SPDX-License-Identifier: GPL-2.0
/* aw_expert_test — option (B): one file per expert, on the dedicated ext4 partition.
 *
 * BACKLOG item 24. `docs/USING_ARCWELL.md` §5 recommended this layout and claimed
 * its payoff in one sentence: "one file per expert, fallocate'd, gives exactly
 * 1 extent per expert and one DMA segment". The first half is true and filefrag
 * shows it. THE SECOND HALF WAS FALSE, and this cell is what caught it.
 *
 * A segment is a bio, and a bio holds at most BIO_MAX_VECS (256) pages = 1 MiB.
 * The experts here are 2.34 MiB, so three bios is the FLOOR no layout can beat;
 * measured, one expert costs four. "One DMA segment per expert" was never
 * achievable for any expert above 1 MiB, which is all of them. The docs said it
 * anyway, and every reader sizing a batch against that sentence would have been
 * wrong. Both `docs/USING_ARCWELL.md` and `README.md` are corrected.
 *
 * What the layout DOES buy is real and worth having: one extent means one
 * contiguous LBA range, so an expert is ONE request. Under option (A) an expert
 * that straddles an extent boundary becomes two requests at different
 * in_dest_offsets. That is the difference; the bio count is the block layer's
 * business and is the same either way.
 *
 * `aw_gguf_test` covers option (A), ranges inside one big shard, and needs an
 * extent boundary to straddle. A single-extent expert file has none, so it could
 * not cover this layout and the recommended one went unmeasured while the
 * fallback was tested.
 *
 * Asserted, each able to fail on its own:
 *   1. every expert file resolves to exactly ONE plain extent;
 *   2. the batch DMAs all of them, and segments scale LINEARLY with expert count
 *      and stay within one bio per expert of the BIO_MAX_VECS floor -- i.e. the
 *      splitting is driven by the bio vector limit, not by the extent layout;
 *   3. every expert's VRAM content equals the file's bytes read through the
 *      ordinary filesystem path;
 *   4. via_host_bounce does not move.
 *
 * NO BYTE-WISE CPU READ OF VRAM. A 4-byte CPU read of a WC VRAM mapping costs
 * ~1.4 us (results/WC_CARVE_2026-09-16.txt), so a byte-wise compare of 2.4 MB
 * would take minutes per expert. Readback is memcpy, which issues wide loads and
 * runs ~5x faster, and the comparison is then done in host memory.
 *
 * RED-FIRST: --mutate drops the partition start offset, exactly the mistake
 * `aw_fiemap.c` was written to catch. The LBAs then land 48 GiB early, inside
 * other partitions, and the DMA SUCCEEDS while returning the wrong bytes. The
 * cell must go red on content, not on an error code -- which is the whole reason
 * the content check exists.
 *
 * Build: gcc -O2 -Wall -I../include -I. -I/usr/include/drm -o aw_expert_test aw_expert_test.c
 * Run:   ./aw_expert_test [--mutate] [--dir DIR] [--part-start SECTORS] [--count N] [--drm PATH]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include "xe_drm.h"
#include "aw_uapi.h"

#define MAXEXP 64

/* fiemap needs a trailing flexible array; a char[] cast to struct fiemap* is
 * misaligned and -O2 vectorises the zero-init into a fault. Union it. */
union fmbuf {
	struct fiemap fm;
	char raw[sizeof(struct fiemap) + 4 * sizeof(struct fiemap_extent)];
};

static int fail(const char *what) { printf("RESULT=FAIL -- %s (%s)\n", what, strerror(errno)); return 1; }

static int cmp_name(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }

int main(int argc, char **argv)
{
	const char *drm = "/dev/dri/renderD128", *dir = "/flash/experts";
	uint64_t part_start = 100665344ULL;
	int mutate = 0, count = 8, i, n = 0;
	char *names[MAXEXP];

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mutate")) mutate = 1;
		else if (!strcmp(argv[i], "--drm") && i + 1 < argc) drm = argv[++i];
		else if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
		else if (!strcmp(argv[i], "--part-start") && i + 1 < argc) part_start = strtoull(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoi(argv[++i]);
	}
	if (count > MAXEXP) count = MAXEXP;

	printf("==== option (B): one file per expert %s ====\n",
	       mutate ? "[MUTATED: partition offset dropped - MUST FAIL ON CONTENT]" : "[normal - must PASS]");
	printf("dir=%s part_start=%llu (%.1f GiB) experts=%d\n",
	       dir, (unsigned long long)part_start, part_start / 2097152.0, count);

	DIR *d = opendir(dir);
	if (!d) return fail("opendir");
	struct dirent *de;
	while ((de = readdir(d)) && n < count)
		if (de->d_name[0] != '.') names[n++] = strdup(de->d_name);
	closedir(d);
	if (n < count) { printf("RESULT=FAIL -- only %d files in %s\n", n, dir); return 1; }
	qsort(names, n, sizeof names[0], cmp_name);

	int gfd = open(drm, O_RDWR | O_CLOEXEC);
	if (gfd < 0) return fail("open drm");
	int afd = open("/dev/" AW_DEVICE_NAME, O_RDWR);
	if (afd < 0) return fail("open /dev/" AW_DEVICE_NAME);
	struct aw_ioc_stats base; memset(&base, 0, sizeof base);
	ioctl(afd, AW_IOC_STATS, &base);

	struct aw_ioc_read_blocks reqs[MAXEXP];
	void *vram[MAXEXP];
	size_t sz[MAXEXP];
	char paths[MAXEXP][512];

	for (i = 0; i < n; i++) {
		union fmbuf fb;
		struct stat st;

		snprintf(paths[i], sizeof paths[i], "%s/%s", dir, names[i]);
		int ffd = open(paths[i], O_RDONLY);
		if (ffd < 0) return fail("open expert file");
		if (fstat(ffd, &st) < 0) return fail("fstat");
		sz[i] = st.st_size;

		/* --- 1. FIEMAP: must be exactly one plain extent --- */
		memset(&fb, 0, sizeof fb);
		fb.fm.fm_start = 0;
		fb.fm.fm_length = st.st_size;
		fb.fm.fm_flags = FIEMAP_FLAG_SYNC;
		fb.fm.fm_extent_count = 4;
		if (ioctl(ffd, FS_IOC_FIEMAP, &fb.fm) < 0) return fail("FS_IOC_FIEMAP");
		close(ffd);

		if (fb.fm.fm_mapped_extents != 1) {
			printf("RESULT=FAIL -- %s has %u extents, the layout promises 1\n",
			       names[i], fb.fm.fm_mapped_extents);
			return 1;
		}
		struct fiemap_extent *ex = &fb.fm.fm_extents[0];
		if (ex->fe_flags & (FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC |
				    FIEMAP_EXTENT_ENCODED | FIEMAP_EXTENT_DATA_INLINE |
				    FIEMAP_EXTENT_UNWRITTEN)) {
			printf("RESULT=FAIL -- %s extent not plain (flags %#llx)\n",
			       names[i], (unsigned long long)ex->fe_flags);
			return 1;
		}

		/* --- 2. absolute LBA. The mutation lives here. --- */
		uint64_t lba = ex->fe_physical / 512 + (mutate ? 0 : part_start);

		/* --- 3. a VRAM BO per expert, 64K-aligned --- */
		struct drm_xe_gem_create bo; memset(&bo, 0, sizeof bo);
		bo.size = (st.st_size + 0xffff) & ~0xffffULL;
		bo.placement = 1u << DRM_XE_MEM_REGION_CLASS_VRAM;
		bo.flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;
		bo.cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;
		if (ioctl(gfd, DRM_IOCTL_XE_GEM_CREATE, &bo) < 0) return fail("GEM_CREATE");

		struct drm_xe_gem_mmap_offset mm; memset(&mm, 0, sizeof mm);
		mm.handle = bo.handle;
		if (ioctl(gfd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mm) < 0) return fail("MMAP_OFFSET");
		vram[i] = mmap(NULL, bo.size, PROT_READ | PROT_WRITE, MAP_SHARED, gfd, mm.offset);
		if (vram[i] == MAP_FAILED) return fail("mmap BO");
		memset(vram[i], 0xA5, bo.size);		/* poison, so a no-op DMA shows */

		struct drm_prime_handle pr; memset(&pr, 0, sizeof pr);
		pr.handle = bo.handle; pr.flags = O_RDWR | O_CLOEXEC;
		if (ioctl(gfd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &pr) < 0) return fail("PRIME");

		struct aw_ioc_map_buffer mb; memset(&mb, 0, sizeof mb);
		mb.in_handle = (uint64_t)pr.fd;
		mb.in_source = AW_BUF_DMABUF;
		mb.in_length = bo.size;
		if (ioctl(afd, AW_IOC_MAP_BUFFER, &mb) < 0) return fail("MAP_BUFFER");
		if (!(mb.out_flags & AW_MAP_F_REQUIRE_P2P)) {
			printf("RESULT=FAIL -- %s mapped without peer-to-peer\n", names[i]);
			return 1;
		}
		close(pr.fd);

		memset(&reqs[i], 0, sizeof reqs[i]);
		reqs[i].in_buffer_handle = mb.out_handle;
		reqs[i].in_start_block = lba;
		reqs[i].in_block_count = st.st_size >> 9;
	}
	printf("%d experts, each exactly 1 plain extent\n", n);

	/* --- 4. one batch, the inference shape --- */
	struct aw_ioc_read_batch batch; memset(&batch, 0, sizeof batch);
	batch.in_requests = (uint64_t)(uintptr_t)reqs;
	batch.in_count = n;
	if (ioctl(afd, AW_IOC_READ_BATCH, &batch) < 0) return fail("AW_IOC_READ_BATCH");
	/* struct aw_ioc_read_batch carries no segment count -- segments live only in
	 * the MODULE-GLOBAL stats. So this is read as a DELTA around the batch, the
	 * same discipline via_host_bounce needs and for the same reason: an absolute
	 * reading includes every other client's work. */
	struct aw_ioc_stats mid; memset(&mid, 0, sizeof mid);
	ioctl(afd, AW_IOC_STATS, &mid);
	uint32_t segs = mid.segments - base.segments;
	printf("batch: completed=%u bytes=%llu segments(delta)=%u err=%d\n",
	       batch.out_completed, (unsigned long long)batch.out_bytes,
	       segs, batch.out_err);

	if (batch.out_completed != (uint32_t)n) {
		printf("RESULT=FAIL -- completed %u of %d\n", batch.out_completed, n);
		return 1;
	}
	/* A bio holds at most BIO_MAX_VECS pages, so this is the floor for ANY
	 * layout. The assertion is that we are near it and that it scales with the
	 * expert count -- not that it is 1, which is unachievable above 1 MiB. */
	unsigned int pages_per = (sz[0] + 4095) / 4096;
	unsigned int floor_per = (pages_per + 255) / 256;
	unsigned int floor = floor_per * (unsigned int)n;
	printf("segments: %u for %d experts (%.2f/expert); BIO_MAX_VECS floor is %u/expert => %u\n",
	       segs, n, (double)segs / n, floor_per, floor);
	if (segs < floor) {
		printf("RESULT=FAIL -- %u segments is below the BIO_MAX_VECS floor of %u; "
		       "the accounting is wrong\n", segs, floor);
		return 1;
	}
	if (segs > floor + (unsigned int)n) {
		printf("RESULT=FAIL -- %u segments against a floor of %u: more than one "
		       "extra bio per expert means something beyond the vector limit is "
		       "splitting these transfers\n", segs, floor);
		return 1;
	}

	/* --- 5. content, via memcpy not a byte-wise read (see header) --- */
	int bad = -1;
	void *host = NULL, *got = NULL;
	for (i = 0; i < n && bad < 0; i++) {
		host = malloc(sz[i]); got = malloc(sz[i]);
		if (!host || !got) return fail("malloc");
		int ffd = open(paths[i], O_RDONLY);
		if (ffd < 0 || read(ffd, host, sz[i]) != (ssize_t)sz[i]) return fail("read expert file");
		close(ffd);
		memcpy(got, vram[i], sz[i]);
		if (memcmp(got, host, sz[i])) bad = i;
		free(host); free(got);
	}

	struct aw_ioc_stats st2; memset(&st2, 0, sizeof st2);
	ioctl(afd, AW_IOC_STATS, &st2);
	if (st2.via_host_bounce != base.via_host_bounce) {
		printf("RESULT=FAIL -- via_host_bounce moved by %u\n",
		       st2.via_host_bounce - base.via_host_bounce);
		return 1;
	}

	if (bad >= 0) {
		printf("RESULT=FAIL -- %s: VRAM does not match the file%s\n", names[bad],
		       mutate ? " (expected: the partition offset was dropped)" : "");
		return 1;
	}
	if (mutate) {
		printf("RESULT=FAIL -- content matched WITHOUT the partition offset; the "
		       "offset is then not doing anything and the red leg proves nothing\n");
		return 1;
	}

	printf("all %d experts verified against the filesystem path; via_host_bounce unmoved\n", n);
	printf("RESULT=PASS -- option (B) DMAs one-file-per-expert from the ext4 partition: "
	       "one extent, one request, %.2f bios/expert against a floor of %u, no host bounce\n",
	       (double)segs / n, floor_per);
	close(afd); close(gfd);
	return 0;
}
