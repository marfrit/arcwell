// SPDX-License-Identifier: GPL-2.0
/* aw_bo_test — BACKLOG item 14 acceptance client.
 *
 * Proves the thing the M0 gate did NOT prove: that NVMe bytes land in a buffer
 * the GPU actually owns. The M0 gate wrote into raw carved BAR2 p2pmem, which is
 * not a GEM BO, so OpenCL/OpenVINO could not have consumed it.
 *
 * Flow: create a host-visible VRAM BO on xe -> export it as a dma-buf -> hand the
 * fd to arcwell via AW_IOC_MAP_BUFFER -> AW_IOC_READ_BLOCKS -> then VERIFY BY
 * READING THE BO THROUGH ITS OWN xe MMAP, not through a raw BAR window.
 *
 * RED-FIRST: --mutate compares against a different disk offset than the one that
 * was read. The DMA still happens, the bytes still land, but the comparand is
 * wrong, so the cell MUST report FAIL. Run it before trusting the green.
 *
 * Build: gcc -O2 -Wall -I../src -I../include -o aw_bo_test aw_bo_test.c
 * Run:   ./aw_bo_test [--mutate] [--drm /dev/dri/renderD128] [--bdev /dev/nvme0n1]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include "xe_drm.h"
#include "aw_uapi.h"

#define BO_SIZE   (1u << 20)		/* 1 MiB, 64K-aligned */
#define DISK_OFF  0x1000000000ULL	/* 64 GiB in: dense real data, read-only */
#define POISON    0xA5

static int die(const char *what) { fprintf(stderr, "FAIL: %s: %s\n", what, strerror(errno)); return 1; }

int main(int argc, char **argv)
{
	const char *drm = "/dev/dri/renderD128", *bdevp = "/dev/nvme0n1";
	int mutate = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mutate")) mutate = 1;
		else if (!strcmp(argv[i], "--drm") && i + 1 < argc) drm = argv[++i];
		else if (!strcmp(argv[i], "--bdev") && i + 1 < argc) bdevp = argv[++i];
	}
	printf("==== arcwell BO acceptance %s ====\n",
	       mutate ? "[MUTATED - MUST FAIL]" : "[normal - must PASS]");
	printf("drm=%s bdev=%s size=%u disk_off=%#llx\n",
	       drm, bdevp, BO_SIZE, (unsigned long long)DISK_OFF);

	int gfd = open(drm, O_RDWR | O_CLOEXEC);
	if (gfd < 0) return die("open drm");

	/* --- 1. host-visible VRAM BO: WC + NEEDS_VISIBLE_VRAM + 64K-aligned --- */
	struct drm_xe_gem_create bo = {0};
	bo.size = BO_SIZE;
	bo.placement = 1u << DRM_XE_MEM_REGION_CLASS_VRAM;
	bo.flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;
	bo.cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;
	if (ioctl(gfd, DRM_IOCTL_XE_GEM_CREATE, &bo) < 0) return die("GEM_CREATE");
	printf("BO created handle=%u\n", bo.handle);

	/* --- 2. map it for the CPU so we can poison and later verify --- */
	struct drm_xe_gem_mmap_offset mm = {0};
	mm.handle = bo.handle;
	if (ioctl(gfd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mm) < 0) return die("MMAP_OFFSET");
	void *vram = mmap(NULL, BO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, gfd, mm.offset);
	if (vram == MAP_FAILED) return die("mmap BO");
	memset(vram, POISON, BO_SIZE);
	__sync_synchronize();
	printf("BO poisoned with 0x%02x through its own xe mapping\n", POISON);

	/* --- 3. export as dma-buf --- */
	struct drm_prime_handle prime = {0};
	prime.handle = bo.handle;
	prime.flags = O_RDWR | O_CLOEXEC;
	if (ioctl(gfd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) return die("PRIME_HANDLE_TO_FD");
	printf("dma-buf fd=%d\n", prime.fd);

	/* --- 4. register with arcwell --- */
	int afd = open("/dev/" AW_DEVICE_NAME, O_RDWR);
	if (afd < 0) return die("open /dev/" AW_DEVICE_NAME);
	struct aw_ioc_stats base = {0}; ioctl(afd, AW_IOC_STATS, &base);

	struct aw_ioc_map_buffer mb = {0};
	mb.in_handle = (uint64_t)prime.fd;
	mb.in_source = AW_BUF_DMABUF;
	mb.in_length = BO_SIZE;
	if (ioctl(afd, AW_IOC_MAP_BUFFER, &mb) < 0) return die("AW_IOC_MAP_BUFFER");
	printf("AW_IOC_MAP_BUFFER ok handle=%u out_flags=%#x%s\n", mb.out_handle, mb.out_flags,
	       (mb.out_flags & AW_MAP_F_REQUIRE_P2P) ? " (REQUIRE_P2P honoured)" : " (!! not peer-to-peer !!)");

	/* --- 5. the transfer: NVMe -> this BO's VRAM --- */
	struct aw_ioc_read_blocks rb = {0};
	rb.in_buffer_handle = mb.out_handle;
	rb.in_start_block = DISK_OFF >> 9;
	rb.in_block_count = BO_SIZE >> 9;
	rb.in_dest_offset = 0;
	if (ioctl(afd, AW_IOC_READ_BLOCKS, &rb) < 0) return die("AW_IOC_READ_BLOCKS");
	printf("AW_IOC_READ_BLOCKS ok bytes=%llu segments=%u\n",
	       (unsigned long long)rb.out_bytes, rb.out_segments);

	/* --- 6. control: same LBAs through the ordinary host path --- */
	uint64_t cmp_off = mutate ? DISK_OFF + BO_SIZE : DISK_OFF;
	void *host = aligned_alloc(4096, BO_SIZE);
	int bfd = open(bdevp, O_RDONLY | O_DIRECT);
	if (bfd < 0 || !host) return die("open bdev");
	if (pread(bfd, host, BO_SIZE, cmp_off) != (ssize_t)BO_SIZE) return die("pread control");
	printf("control read from %#llx%s\n", (unsigned long long)cmp_off,
	       mutate ? "  <-- MUTATED COMPARAND" : "");

	/* --- 7. VERDICT: read the BO back through the GPU's own mapping --- */
	int poisoned = 1;
	for (unsigned i = 0; i < BO_SIZE; i++)
		if (((unsigned char *)vram)[i] != POISON) { poisoned = 0; break; }
	int match = memcmp(vram, host, BO_SIZE) == 0;

	struct aw_ioc_stats st = {0};
	ioctl(afd, AW_IOC_STATS, &st);
	printf("stats: reads=%llu bytes=%llu us=%llu segments=%u via_host_bounce=%u\n",
	       (unsigned long long)st.reads, (unsigned long long)st.bytes,
	       (unsigned long long)st.us_total, st.segments, st.via_host_bounce);

	if (st.via_host_bounce != base.via_host_bounce)  /* DELTA: the counter is module-global */ { printf("RESULT=FAIL -- via_host_bounce moved by %u: a mapping was refused as a host bounce\n", st.via_host_bounce - base.via_host_bounce); return 1; }
	if (poisoned)           { printf("RESULT=FAIL -- BO still poisoned; nothing landed in it\n"); return 1; }
	if (!match)             { printf("RESULT=FAIL -- BO content does not match disk\n"); return 1; }
	printf("RESULT=PASS -- disk bytes landed in a GPU-owned BO, verified through its xe mapping\n");
	return 0;
}
