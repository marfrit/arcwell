// SPDX-License-Identifier: GPL-2.0
/* aw_fiemap — expert file -> absolute NVMe LBA, and a cell that proves it.
 *
 * M4_API.md: the module takes RAW BLOCK RANGES; FIEMAP is a once-at-open
 * userspace translation, not part of the ioctl surface. This is that translation.
 *
 * THE OFF-BY-ONE THIS EXISTS TO PREVENT. FS_IOC_FIEMAP returns `fe_physical` as a
 * byte offset INSIDE THE FILESYSTEM. The expert store is nvme0n1p3, which starts
 * at sector 100665344 (48.0 GiB). Absolute LBA = fe_physical/512 + part_start.
 * Forget the partition start and every read lands 48 GiB early -- inside rpool's
 * SLOG and L2ARC partitions -- and returns plausible garbage rather than an error.
 *
 * The cell: write a known pattern to each file, fsync, FIEMAP it, then read the
 * RAW DEVICE at the computed LBA and compare. --mutate drops the partition offset,
 * which is exactly the mistake above, and MUST fail.
 *
 * Extents are rejected unless they are plain: UNWRITTEN, DELALLOC, INLINE,
 * ENCODED and UNKNOWN all mean the bytes are not where FIEMAP says yet.
 *
 * Build: gcc -O2 -Wall -o aw_fiemap aw_fiemap.c
 * Run:   ./aw_fiemap --dir /flash/experts --dev /dev/nvme0n1 --part-start 100665344
 *                    [--count 8] [--mutate]
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
#include <sys/stat.h>
#include <linux/fs.h>
#include <linux/fiemap.h>

#define SLOT 2457600u          /* per-expert slice */
#define MAXEXT 64

static const char *flagname(uint32_t f)
{
	if (f & FIEMAP_EXTENT_UNKNOWN)   return "UNKNOWN";
	if (f & FIEMAP_EXTENT_DELALLOC)  return "DELALLOC";
	if (f & FIEMAP_EXTENT_ENCODED)   return "ENCODED";
	if (f & FIEMAP_EXTENT_DATA_INLINE) return "INLINE";
	if (f & FIEMAP_EXTENT_UNWRITTEN) return "UNWRITTEN";
	return NULL;
}

int main(int argc, char **argv)
{
	const char *dir = "/flash/experts", *dev = "/dev/nvme0n1";
	unsigned long long part_start = 100665344ULL;
	int count = 8, mutate = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dir") && i+1 < argc) dir = argv[++i];
		else if (!strcmp(argv[i], "--dev") && i+1 < argc) dev = argv[++i];
		else if (!strcmp(argv[i], "--part-start") && i+1 < argc) part_start = strtoull(argv[++i],0,0);
		else if (!strcmp(argv[i], "--count") && i+1 < argc) count = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mutate")) mutate = 1;
	}
	printf("==== expert file -> absolute LBA %s ====\n",
	       mutate ? "[MUTATED: partition offset dropped - MUST FAIL]" : "[normal - must PASS]");
	printf("dir=%s dev=%s part_start=%llu (%.1f GiB) slot=%u count=%d\n",
	       dir, dev, part_start, part_start*512.0/(1<<30), SLOT, count);

	int rawfd = open(dev, O_RDONLY | O_DIRECT);
	if (rawfd < 0) { perror("open raw dev"); return 1; }
	void *rawbuf, *filebuf;
	if (posix_memalign(&rawbuf, 4096, SLOT) || posix_memalign(&filebuf, 4096, SLOT)) return 1;

	unsigned total_ext = 0, checked = 0;
	for (int n = 0; n < count; n++) {
		char path[512];
		snprintf(path, sizeof path, "%s/expert_%04d.bin", dir, n);

		int fd = open(path, O_RDONLY);
		if (fd < 0) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return 1; }
		if (pread(fd, filebuf, SLOT, 0) != (ssize_t)SLOT) { perror("pread file"); return 1; }

		/* union, not char[]: a char array has alignment 1 and -O2 may vectorise
		 * the zero-init into aligned stores on a struct that needs 8. This file
		 * happened to survive it; stub/test/aw_bp.c did not. */
		union { struct fiemap fm; char raw[sizeof(struct fiemap) + MAXEXT*sizeof(struct fiemap_extent)]; } u;
		struct fiemap *fm = &u.fm;
		memset(&u, 0, sizeof u);
		fm->fm_start = 0; fm->fm_length = SLOT;
		fm->fm_flags = FIEMAP_FLAG_SYNC;     /* flush before mapping */
		fm->fm_extent_count = MAXEXT;
		if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) { perror("FS_IOC_FIEMAP"); return 1; }
		if (fm->fm_mapped_extents == 0) { printf("RESULT=FAIL -- %s has no extents\n", path); return 1; }
		total_ext += fm->fm_mapped_extents;

		for (unsigned e = 0; e < fm->fm_mapped_extents; e++) {
			struct fiemap_extent *ex = &fm->fm_extents[e];
			const char *bad = flagname(ex->fe_flags);
			if (bad) { printf("RESULT=FAIL -- %s extent %u is %s; bytes are not there yet\n", path, e, bad); return 1; }

			unsigned long long lba = ex->fe_physical / 512 + (mutate ? 0ULL : part_start);
			unsigned long long len = ex->fe_length;
			if (len > SLOT) len = SLOT;

			if (pread(rawfd, rawbuf, len, lba * 512ULL) != (ssize_t)len) { perror("pread raw"); return 1; }
			if (memcmp(rawbuf, (char*)filebuf + ex->fe_logical, len) != 0) {
				printf("  %s ext%u logical=%llu phys=%llu -> lba=%llu len=%llu  MISMATCH\n",
				       path, e, (unsigned long long)ex->fe_logical,
				       (unsigned long long)ex->fe_physical, lba, len);
				printf("RESULT=FAIL -- raw device at the computed LBA is not the file's bytes\n");
				return 1;
			}
			if (n == 0)
				printf("  ext%u logical=%llu phys=%llu -> ABSOLUTE LBA %llu len=%llu  match\n",
				       e, (unsigned long long)ex->fe_logical,
				       (unsigned long long)ex->fe_physical, lba, len);
			checked++;
		}
		close(fd);
	}
	printf("%d files, %u extents total (%.2f per file), %u verified against the raw device\n",
	       count, total_ext, (double)total_ext/count, checked);
	printf("RESULT=PASS -- FIEMAP + partition offset gives the correct absolute LBA\n");
	return 0;
}
