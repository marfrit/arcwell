// SPDX-License-Identifier: GPL-2.0
/* aw_wc_test — the deciding cell for BACKLOG item 20.
 *
 * CLAIM UNDER TEST (recorded as a HYPOTHESIS in results/E2E_2026-09-15.txt, never
 * confirmed): "a 128 MiB p2pdma carve destroys write-combining for BOs allocated
 * inside it", inferred from a CPU-side verification loop that went from seconds to
 * twelve minutes when the carve granule grew.
 *
 * That inference has an obvious confound: the twelve-minute loop and the seconds
 * loop were not the same loop over the same BO. This cell removes the confound by
 * timing ONE mapping of ONE BO before and after the carve that covers it.
 *
 *   t_before  CPU read rate through the BO's xe mmap, no arcwell involvement
 *   t_after   the same read, same pointer, after AW_IOC_MAP_BUFFER has carved
 *
 * If the hypothesis holds, t_after collapses by orders of magnitude. If the rates
 * match, the hypothesis is disproved and the twelve minutes had another cause.
 *
 * The probe is deliberately small (64 KiB default): at the postulated ~2-4 us per
 * uncached 4-byte read a 1 MiB scan would take minutes, which is what made the
 * original observation so expensive. 64 KiB bounds the worst case at ~65 ms.
 *
 * It also reports a memcpy rate beside the 4-byte loop, because the two differ by
 * two orders of magnitude on a WC mapping and the original observation compared
 * one against the other without saying so.
 *
 * CONTROL: point --drm at a GPU arcwell has never carved (check
 * /sys/bus/pci/devices/<bdf>/p2pmem/size is absent). If an uncarved card reads at
 * the same rate, the carve is exonerated by a second, independent route.
 *
 * RED-FIRST: --mutate divides the reported "after" rate by 1000, fabricating
 * exactly the slowdown the hypothesis predicts. The verdict MUST then flip to
 * CONFIRMED. If it does not, the verdict is hardcoded rather than computed and
 * the green means nothing. Run it first.
 *
 * Build: gcc -O2 -Wall -I/usr/include/drm -o aw_wc_test aw_wc_test.c
 * Run:   ./aw_wc_test [--mutate] [--size BYTES] [--probe BYTES] [--drm PATH]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include "xe_drm.h"
#include "aw_uapi.h"

#define REPS 3
/* A slowdown this large or larger is the hypothesis holding. Chosen well below
 * the ~400x the twelve-minutes-vs-seconds observation implies, and well above
 * any plausible run-to-run noise on an uncached PCIe read. */
#define DEGRADE_FACTOR 10.0

static int die(const char *what) { fprintf(stderr, "FAIL: %s: %s\n", what, strerror(errno)); return 1; }

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Sequential 4-byte reads, the exact shape of the verification loop that hung.
 * volatile so -O2 cannot vectorise or elide it; the sum is returned so it cannot
 * be dead-coded either. */
static double read_rate_mbs(volatile uint32_t *p, size_t bytes, uint64_t *sum_out)
{
	double best = 0;
	for (int r = 0; r < REPS; r++) {
		uint64_t sum = 0;
		double t0 = now();
		for (size_t i = 0; i < bytes / 4; i++)
			sum += p[i];
		double dt = now() - t0;
		double mbs = (bytes / 1048576.0) / dt;
		if (mbs > best) best = mbs;
		*sum_out = sum;
	}
	return best;
}

/* The same bytes via memcpy, which issues wide loads the 4-byte loop cannot. */
static double memcpy_rate_mbs(void *src, size_t bytes, void *dst)
{
	double best = 0;
	for (int r = 0; r < REPS; r++) {
		double t0 = now();
		memcpy(dst, src, bytes);
		double dt = now() - t0;
		double mbs = (bytes / 1048576.0) / dt;
		if (mbs > best) best = mbs;
	}
	return best;
}

int main(int argc, char **argv)
{
	const char *drm = "/dev/dri/renderD128";
	size_t bo_size = 1u << 20, probe = 64u << 10;
	int mutate = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mutate")) mutate = 1;
		else if (!strcmp(argv[i], "--drm") && i + 1 < argc) drm = argv[++i];
		else if (!strcmp(argv[i], "--size") && i + 1 < argc) bo_size = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--probe") && i + 1 < argc) probe = strtoul(argv[++i], NULL, 0);
	}
	if (probe > bo_size) probe = bo_size;

	printf("==== arcwell item-20 WC cell %s ====\n",
	       mutate ? "[MUTATED - verdict must flip to CLEAN on a real slowdown]" : "[normal]");
	printf("drm=%s bo=%zu probe=%zu reps=%d\n", drm, bo_size, probe, REPS);

	int gfd = open(drm, O_RDWR | O_CLOEXEC);
	if (gfd < 0) return die("open drm");

	struct drm_xe_gem_create bo = {0};
	bo.size = bo_size;
	bo.placement = 1u << DRM_XE_MEM_REGION_CLASS_VRAM;
	bo.flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;
	bo.cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;
	if (ioctl(gfd, DRM_IOCTL_XE_GEM_CREATE, &bo) < 0) return die("GEM_CREATE");

	struct drm_xe_gem_mmap_offset mm = {0};
	mm.handle = bo.handle;
	if (ioctl(gfd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mm) < 0) return die("MMAP_OFFSET");
	volatile uint32_t *vram = mmap(NULL, bo_size, PROT_READ | PROT_WRITE, MAP_SHARED, gfd, mm.offset);
	if ((void *)vram == MAP_FAILED) return die("mmap BO");

	/* Touch it so the mapping is fully faulted in before either measurement. */
	memset((void *)vram, 0x5A, bo_size);
	__sync_synchronize();

	void *scratch = malloc(probe);
	if (!scratch) return die("malloc scratch");

	uint64_t s1 = 0, s2 = 0;
	double before = read_rate_mbs(vram, probe, &s1);
	double mc_before = memcpy_rate_mbs((void *)vram, probe, scratch);
	printf("t_before: %8.2f MB/s  4-byte loop   |  %8.2f MB/s  memcpy\n", before, mc_before);

	/* --- now let arcwell carve the range this BO lives in --- */
	struct drm_prime_handle prime = {0};
	prime.handle = bo.handle;
	prime.flags = O_RDWR | O_CLOEXEC;
	if (ioctl(gfd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) return die("PRIME_HANDLE_TO_FD");

	int afd = open("/dev/" AW_DEVICE_NAME, O_RDWR);
	if (afd < 0) return die("open /dev/" AW_DEVICE_NAME);

	struct aw_ioc_map_buffer mb = {0};
	mb.in_handle = (uint64_t)prime.fd;
	mb.in_source = AW_BUF_DMABUF;
	mb.in_length = bo_size;
	if (ioctl(afd, AW_IOC_MAP_BUFFER, &mb) < 0) return die("AW_IOC_MAP_BUFFER");
	printf("AW_IOC_MAP_BUFFER ok handle=%u out_flags=%#x\n", mb.out_handle, mb.out_flags);

	double after = read_rate_mbs(vram, probe, &s2);
	double mc_after = memcpy_rate_mbs((void *)vram, probe, scratch);
	if (mutate) after /= 1000.0;
	printf("t_after:  %8.2f MB/s  4-byte loop   |  %8.2f MB/s  memcpy%s\n",
	       after, mc_after, mutate ? "   <-- MUTATED: 'after' divided by 1000" : "");
	printf("          (same pointer, same loop, BO now inside a p2pdma carve)\n");

	double factor = after > 0 ? before / after : 1e9;
	printf("\nsums: %#llx / %#llx  (read, not elided)\n",
	       (unsigned long long)s1, (unsigned long long)s2);
	printf("slowdown factor: %.2fx  (threshold %.0fx)\n", factor, DEGRADE_FACTOR);
	printf("memcpy vs 4-byte loop on the SAME mapping: %.0fx\n",
	       before > 0 ? mc_before / before : 0.0);

	int degraded = factor >= DEGRADE_FACTOR;
	printf("\nVERDICT: item 20 is %s\n",
	       degraded ? "CONFIRMED — the carve degrades the CPU mapping"
			: "NOT REPRODUCED — the carve does not change the CPU read rate");

	__u32 h = mb.out_handle;
	ioctl(afd, AW_IOC_UNMAP_BUFFER, &h);
	free(scratch);
	close(afd); close(prime.fd); close(gfd);
	return degraded ? 2 : 0;
}
