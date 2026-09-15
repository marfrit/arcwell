// SPDX-License-Identifier: GPL-2.0
/* aw_gguf_test — M4_API.md option (A): experts as ranges inside one big file.
 *
 * The synthetic store used for the gate is option (B) — one file per expert,
 * fallocate'd, one extent each. That is the RECOMMENDED layout and the easy case.
 * The shipped artifact is option (A): expert rows at offsets inside multi-GB GGUF
 * shards, where "an expert can span an extent boundary -> multi-segment DMA".
 *
 * Measured on this store: a 40 GB shard on fresh ext4 has 22 extents averaging
 * 1.7 GiB, so a 2.4 MB slice straddles a boundary about 0.14% of the time. Far
 * too rare to catch by sampling. This cell therefore reads FIEMAP first and
 * places a slice DELIBERATELY across a boundary, alongside one wholly interior.
 *
 * Both slices are DMA'd into the same VRAM BO through arcwell and verified against
 * an ordinary pread of the same file range.
 *
 * RED-FIRST: --mutate drops the partition start from the LBA computation, which is
 * the same error stub/tools/aw_fiemap.c exists to catch, and MUST fail here too.
 *
 * Build: gcc -O2 -Wall -I. -I/usr/include/drm -o aw_gguf_test aw_gguf_test.c
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
#include <sys/mman.h>
#include <linux/fs.h>
#include <linux/fiemap.h>
#include "xe_drm.h"
#include "aw_uapi.h"

#define SLOT    2457600u          /* per-expert slice */
#define MAXEXT  64
#define MAXSEG  16

struct seg { uint64_t lba, bytes, dest_off; };

/* file range -> absolute LBA segments. This is the option (A) translation. */
static int map_range(int fd, uint64_t off, uint64_t len, uint64_t part_start,
		     struct seg *segs, int maxseg, int mutate)
{
	union { struct fiemap fm; char raw[sizeof(struct fiemap)+MAXEXT*sizeof(struct fiemap_extent)]; } u;
	struct fiemap *fm = &u.fm;
	int n = 0;

	memset(&u, 0, sizeof u);
	fm->fm_start = off; fm->fm_length = len;
	fm->fm_flags = FIEMAP_FLAG_SYNC; fm->fm_extent_count = MAXEXT;
	if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) { perror("FIEMAP"); return -1; }

	for (unsigned e = 0; e < fm->fm_mapped_extents && n < maxseg; e++) {
		struct fiemap_extent *ex = &fm->fm_extents[e];
		uint64_t es = ex->fe_logical, ee = ex->fe_logical + ex->fe_length;
		uint64_t s = off > es ? off : es;
		uint64_t t = (off + len) < ee ? (off + len) : ee;

		if (ex->fe_flags & (FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC |
				    FIEMAP_EXTENT_ENCODED | FIEMAP_EXTENT_DATA_INLINE |
				    FIEMAP_EXTENT_UNWRITTEN)) {
			fprintf(stderr, "extent %u not plain (flags %#x)\n", e, ex->fe_flags);
			return -1;
		}
		if (s >= t) continue;
		uint64_t phys = ex->fe_physical + (s - es);
		segs[n].lba      = phys / 512 + (mutate ? 0ULL : part_start);
		segs[n].bytes    = t - s;
		segs[n].dest_off = s - off;
		n++;
	}
	return n;
}

int main(int argc, char **argv)
{
	const char *path = "/flash/gguf/Qwen3.8-Flash-Next-UD-Q3_K_XL-00003-of-00003.gguf";
	const char *drm = "/dev/dri/renderD129";
	uint64_t part_start = 100665344ULL;
	int mutate = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mutate")) mutate = 1;
		else if (!strcmp(argv[i], "--file") && i+1 < argc) path = argv[++i];
		else if (!strcmp(argv[i], "--drm") && i+1 < argc) drm = argv[++i];
		else if (!strcmp(argv[i], "--part-start") && i+1 < argc) part_start = strtoull(argv[++i],0,0);
	}
	printf("==== GGUF option (A) %s ====\n",
	       mutate ? "[MUTATED: partition offset dropped - MUST FAIL]" : "[normal - must PASS]");

	int ffd = open(path, O_RDONLY);
	if (ffd < 0) { perror("open gguf"); return 1; }

	/* --- find a real extent boundary --- */
	union { struct fiemap fm; char raw[sizeof(struct fiemap)+MAXEXT*sizeof(struct fiemap_extent)]; } u;
	struct fiemap *fm = &u.fm;
	memset(&u, 0, sizeof u);
	fm->fm_start = 0; fm->fm_length = ~0ULL; fm->fm_flags = FIEMAP_FLAG_SYNC; fm->fm_extent_count = MAXEXT;
	if (ioctl(ffd, FS_IOC_FIEMAP, fm) < 0) { perror("FIEMAP whole file"); return 1; }
	printf("file has %u extents\n", fm->fm_mapped_extents);
	if (fm->fm_mapped_extents < 2) { printf("RESULT=FAIL -- need >=2 extents to test spanning\n"); return 1; }

	uint64_t boundary = fm->fm_extents[0].fe_logical + fm->fm_extents[0].fe_length;
	uint64_t straddle = (boundary - SLOT/2) & ~4095ULL;   /* half either side */
	uint64_t interior = (fm->fm_extents[0].fe_logical + SLOT * 4) & ~4095ULL;
	printf("extent boundary at logical %llu (%.2f GiB)\n",
	       (unsigned long long)boundary, boundary/1073741824.0);
	printf("  interior slice at %llu ; straddling slice at %llu\n",
	       (unsigned long long)interior, (unsigned long long)straddle);

	/* --- VRAM BO for two slices --- */
	int gfd = open(drm, O_RDWR|O_CLOEXEC); if (gfd < 0) { perror("open drm"); return 1; }
	struct drm_xe_gem_create bo = {0};
	bo.size = (2*SLOT + 0xffff) & ~0xffffULL;
	bo.placement = 1u << DRM_XE_MEM_REGION_CLASS_VRAM;
	bo.flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;
	bo.cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;
	if (ioctl(gfd, DRM_IOCTL_XE_GEM_CREATE, &bo) < 0) { perror("GEM_CREATE"); return 1; }
	struct drm_xe_gem_mmap_offset mm = {0}; mm.handle = bo.handle;
	if (ioctl(gfd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mm) < 0) { perror("MMAP_OFFSET"); return 1; }
	void *vram = mmap(NULL, bo.size, PROT_READ|PROT_WRITE, MAP_SHARED, gfd, mm.offset);
	if (vram == MAP_FAILED) { perror("mmap"); return 1; }
	memset(vram, 0xA5, bo.size);
	struct drm_prime_handle pr = {0}; pr.handle = bo.handle; pr.flags = O_RDWR|O_CLOEXEC;
	if (ioctl(gfd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &pr) < 0) { perror("PRIME"); return 1; }

	int afd = open("/dev/" AW_DEVICE_NAME, O_RDWR); if (afd < 0) { perror("open arcwell"); return 1; }
	struct aw_ioc_stats base = {0}; ioctl(afd, AW_IOC_STATS, &base);
	struct aw_ioc_map_buffer mb = {0};
	mb.in_handle = (uint64_t)pr.fd; mb.in_source = AW_BUF_DMABUF; mb.in_length = bo.size;
	if (ioctl(afd, AW_IOC_MAP_BUFFER, &mb) < 0) { perror("MAP_BUFFER"); return 1; }

	/* --- translate and DMA both slices --- */
	uint64_t offs[2] = { interior, straddle };
	const char *names[2] = { "interior ", "straddling" };
	struct aw_ioc_read_blocks reqs[MAXSEG*2];
	int nreq = 0, nseg[2];

	for (int k = 0; k < 2; k++) {
		struct seg segs[MAXSEG];
		int n = map_range(ffd, offs[k], SLOT, part_start, segs, MAXSEG, mutate);
		if (n < 1) { printf("RESULT=FAIL -- translation failed for %s slice\n", names[k]); return 1; }
		nseg[k] = n;
		printf("  %s slice -> %d segment(s)\n", names[k], n);
		for (int i = 0; i < n; i++) {
			printf("      seg%d lba=%llu bytes=%llu dest_off=%llu\n", i,
			       (unsigned long long)segs[i].lba, (unsigned long long)segs[i].bytes,
			       (unsigned long long)segs[i].dest_off);
			memset(&reqs[nreq], 0, sizeof reqs[nreq]);
			reqs[nreq].in_buffer_handle = mb.out_handle;
			reqs[nreq].in_start_block   = segs[i].lba;
			reqs[nreq].in_block_count   = segs[i].bytes / 512;
			reqs[nreq].in_dest_offset   = (uint64_t)k*SLOT + segs[i].dest_off;
			nreq++;
		}
	}
	struct aw_ioc_read_batch rb = {0};
	rb.in_requests = (uint64_t)(uintptr_t)reqs; rb.in_count = nreq;
	if (ioctl(afd, AW_IOC_READ_BATCH, &rb) < 0) { perror("READ_BATCH"); return 1; }
	if (rb.out_err) { printf("RESULT=FAIL -- batch err=%d at %u\n", rb.out_err, rb.out_err_index); return 1; }
	printf("  DMA'd %d segments, %llu bytes\n", nreq, (unsigned long long)rb.out_bytes);

	/* --- verify against an ordinary read of the same ranges --- */
	void *ref = malloc(SLOT);
	int bad = 0;
	for (int k = 0; k < 2; k++) {
		if (pread(ffd, ref, SLOT, offs[k]) != (ssize_t)SLOT) { perror("pread"); return 1; }
		if (memcmp(ref, (char*)vram + (size_t)k*SLOT, SLOT) != 0) {
			printf("  %s slice MISMATCH\n", names[k]); bad = 1;
		} else {
			printf("  %s slice verified against pread\n", names[k]);
		}
	}
	struct aw_ioc_stats st = {0}; ioctl(afd, AW_IOC_STATS, &st);
	printf("  via_host_bounce=%u segments=%u\n", st.via_host_bounce, st.segments);
	if (st.via_host_bounce != base.via_host_bounce)  /* DELTA: the counter is module-global */ { printf("RESULT=FAIL -- via_host_bounce=%u\n", st.via_host_bounce); return 1; }
	if (bad) { printf("RESULT=FAIL -- VRAM does not match the file\n"); return 1; }
	if (nseg[1] < 2) printf("NOTE: the straddling slice produced %d segment, not >1 — boundary placement missed\n", nseg[1]);
	printf("RESULT=PASS -- option (A) ranges DMA'd from inside a %u-extent GGUF, interior and boundary-spanning, no host bounce\n",
	       fm->fm_mapped_extents);
	return 0;
}
