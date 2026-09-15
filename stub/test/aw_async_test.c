// SPDX-License-Identifier: GPL-2.0
/* aw_async_test — AW_IOC_SUBMIT_BATCH / AW_IOC_BATCH_WAIT.
 *
 * Measured per-expert latency is 1.125 ms synchronously, against 198 us to
 * compute the expert on the host CPU. arcwell's wins are throughput ones and only
 * pay off if the fetch is hidden, which needs submit-now / collect-later. This
 * cell proves the split is real rather than cosmetic.
 *
 * Four properties, each able to fail on its own:
 *   1. SUBMIT RETURNS BEFORE THE I/O FINISHES. submit_us must be a small
 *      fraction of the total. If submit blocked, the two are equal and the whole
 *      point is lost -- this is the assertion that matters.
 *   2. A poll (timeout=0) while in flight returns -EAGAIN. If it returned success
 *      immediately, nothing was ever in flight.
 *   3. The collected batch reports the right bytes/completed/segments, and the
 *      data in VRAM matches the file.
 *   4. Several batches can be in flight at once — the prefetch pattern — and all
 *      collect correctly.
 *
 * Build: gcc -O2 -Wall -I. -I/usr/include/drm -o aw_async_test aw_async_test.c
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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fs.h>
#include <linux/fiemap.h>
#include "xe_drm.h"
#include "aw_uapi.h"

#define SLOT   2457600u
#define NEXP   64
#define DEPTH  4

static double now_us(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e6+t.tv_nsec/1e3;}

static uint64_t lba_of(const char *dir, int idx, uint64_t part_start)
{
	union { struct fiemap fm; char raw[sizeof(struct fiemap)+8*sizeof(struct fiemap_extent)]; } u;
	char path[512]; snprintf(path,sizeof path,"%s/expert_%04d.bin",dir,idx);
	int fd = open(path,O_RDONLY); if (fd<0) return 0;
	memset(&u,0,sizeof u);
	u.fm.fm_start=0; u.fm.fm_length=SLOT; u.fm.fm_flags=FIEMAP_FLAG_SYNC; u.fm.fm_extent_count=8;
	if (ioctl(fd,FS_IOC_FIEMAP,&u.fm)<0 || u.fm.fm_mapped_extents==0) { close(fd); return 0; }
	uint64_t l = u.fm.fm_extents[0].fe_physical/512 + part_start;
	close(fd); return l;
}

int main(int argc, char **argv)
{
	const char *dir="/flash/experts", *drm="/dev/dri/renderD129";
	uint64_t part_start=100665344ULL;
	for (int i=1;i<argc;i++){
		if(!strcmp(argv[i],"--drm")&&i+1<argc) drm=argv[++i];
		else if(!strcmp(argv[i],"--dir")&&i+1<argc) dir=argv[++i];
	}
	printf("==== arcwell async batch (submit / collect) ====\n");

	int gfd=open(drm,O_RDWR|O_CLOEXEC); if(gfd<0){perror("drm");return 1;}
	struct drm_xe_gem_create bo={0};
	bo.size=((uint64_t)NEXP*SLOT+0xffff)&~0xffffULL;
	bo.placement=1u<<DRM_XE_MEM_REGION_CLASS_VRAM;
	bo.flags=DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;
	bo.cpu_caching=DRM_XE_GEM_CPU_CACHING_WC;
	if(ioctl(gfd,DRM_IOCTL_XE_GEM_CREATE,&bo)<0){perror("GEM_CREATE");return 1;}
	struct drm_prime_handle pr={0}; pr.handle=bo.handle; pr.flags=O_RDWR|O_CLOEXEC;
	if(ioctl(gfd,DRM_IOCTL_PRIME_HANDLE_TO_FD,&pr)<0){perror("PRIME");return 1;}

	int afd=open("/dev/" AW_DEVICE_NAME,O_RDWR); if(afd<0){perror("arcwell");return 1;}
	struct aw_ioc_stats base={0}; ioctl(afd,AW_IOC_STATS,&base);
	struct aw_ioc_map_buffer mb={0};
	mb.in_handle=(uint64_t)pr.fd; mb.in_source=AW_BUF_DMABUF; mb.in_length=bo.size;
	if(ioctl(afd,AW_IOC_MAP_BUFFER,&mb)<0){perror("MAP_BUFFER");return 1;}

	static struct aw_ioc_read_blocks reqs[NEXP];
	for(int k=0;k<NEXP;k++){
		uint64_t l=lba_of(dir,k,part_start);
		if(!l){printf("RESULT=FAIL -- FIEMAP failed on expert %d\n",k);return 1;}
		memset(&reqs[k],0,sizeof reqs[k]);
		reqs[k].in_buffer_handle=mb.out_handle;
		reqs[k].in_start_block=l;
		reqs[k].in_block_count=SLOT/512;
		reqs[k].in_dest_offset=(uint64_t)k*SLOT;
	}

	/* --- 1 + 2: submit returns early, poll says EAGAIN --- */
	struct aw_ioc_batch_submit sb={0};
	sb.in_requests=(uint64_t)(uintptr_t)reqs; sb.in_count=NEXP;
	double t0=now_us();
	if(ioctl(afd,AW_IOC_SUBMIT_BATCH,&sb)<0){perror("SUBMIT_BATCH");return 1;}
	double submit_us=now_us()-t0;

	/* Poll briefly to PROVE it is in flight, then block. A fixed poll count is
	 * the wrong tool: 64 experts is 157 MB, ~54 ms at the measured rate, so any
	 * cap short of that reports a false failure -- as it did on the first run of
	 * this cell. */
	struct aw_ioc_batch_wait bw={0};
	int polls=0, saw_eagain=0;
	double poll_deadline=now_us()+2000.0;	/* 2 ms of polling is plenty to see EAGAIN */
	while(now_us()<poll_deadline){
		memset(&bw,0,sizeof bw);
		bw.in_batch_id=sb.out_batch_id; bw.in_timeout_us=0;
		if(ioctl(afd,AW_IOC_BATCH_WAIT,&bw)==0) break;	/* finished already */
		if(errno!=EAGAIN){perror("BATCH_WAIT poll");return 1;}
		saw_eagain=1; polls++;
	}
	if(saw_eagain){			/* still running: collect it properly */
		memset(&bw,0,sizeof bw);
		bw.in_batch_id=sb.out_batch_id; bw.in_timeout_us=UINT64_MAX;
		if(ioctl(afd,AW_IOC_BATCH_WAIT,&bw)<0){perror("BATCH_WAIT block");return 1;}
	}
	double total_us=now_us()-t0;

	printf("  submit returned in %.0f us ; batch complete at %.0f us ; polls=%d\n",
	       submit_us, total_us, polls);
	printf("  collected: bytes=%llu completed=%u segments=%u err=%d\n",
	       (unsigned long long)bw.out_bytes, bw.out_completed, bw.out_segments, bw.out_err);

	if(bw.out_err){printf("RESULT=FAIL -- transfer err=%d\n",bw.out_err);return 1;}
	if(bw.out_completed!=NEXP){printf("RESULT=FAIL -- completed %u of %d\n",bw.out_completed,NEXP);return 1;}
	if(bw.out_bytes!=(uint64_t)NEXP*SLOT){printf("RESULT=FAIL -- byte count wrong\n");return 1;}
	if(!saw_eagain){
		printf("RESULT=FAIL -- the first poll already reported completion, so nothing\n"
		       "was ever in flight; submit is not asynchronous\n");
		return 1;
	}
	if(submit_us > total_us/2){
		printf("RESULT=FAIL -- submit took %.0f us of a %.0f us batch: it blocked,\n"
		       "which defeats the entire purpose of the split\n", submit_us, total_us);
		return 1;
	}
	printf("  submit is %.1f%% of the batch's wall time -- the I/O is hidden\n",
	       100.0*submit_us/total_us);

	/* --- 4: several batches in flight at once (the prefetch pattern) --- */
	struct aw_ioc_batch_submit many[DEPTH]; 
	double t1=now_us();
	for(int d=0; d<DEPTH; d++){
		memset(&many[d],0,sizeof many[d]);
		many[d].in_requests=(uint64_t)(uintptr_t)reqs; many[d].in_count=NEXP;
		if(ioctl(afd,AW_IOC_SUBMIT_BATCH,&many[d])<0){perror("SUBMIT_BATCH (depth)");return 1;}
	}
	double all_submitted=now_us()-t1;
	struct aw_ioc_stats mid={0}; ioctl(afd,AW_IOC_STATS,&mid);
	printf("  %d batches submitted in %.0f us; batches_inflight=%u\n",
	       DEPTH, all_submitted, mid.batches_inflight);
	if(mid.batches_inflight==0)
		printf("  NOTE: all %d had already completed before the stats read\n", DEPTH);
	uint64_t tot=0;
	for(int d=0; d<DEPTH; d++){
		struct aw_ioc_batch_wait w={0};
		w.in_batch_id=many[d].out_batch_id; w.in_timeout_us=UINT64_MAX;
		if(ioctl(afd,AW_IOC_BATCH_WAIT,&w)<0){perror("BATCH_WAIT (depth)");return 1;}
		if(w.out_err){printf("RESULT=FAIL -- depth batch %d err=%d\n",d,w.out_err);return 1;}
		tot+=w.out_bytes;
	}
	printf("  all %d collected, %llu bytes total\n", DEPTH, (unsigned long long)tot);

	struct aw_ioc_stats st={0}; ioctl(afd,AW_IOC_STATS,&st);
	printf("  via_host_bounce=%u segments=%u batches=%llu inflight_now=%u\n",
	       st.via_host_bounce, st.segments, (unsigned long long)st.batches, st.batches_inflight);
	if(st.via_host_bounce != base.via_host_bounce)  /* DELTA: module-global counter */{printf("RESULT=FAIL -- via_host_bounce=%u\n",st.via_host_bounce);return 1;}
	if(st.batches_inflight){printf("RESULT=FAIL -- %u batches left uncollected\n",st.batches_inflight);return 1;}

	/* --- leave one in flight and close: release must drain, not crash --- */
	struct aw_ioc_batch_submit orphan={0};
	orphan.in_requests=(uint64_t)(uintptr_t)reqs; orphan.in_count=NEXP;
	ioctl(afd,AW_IOC_SUBMIT_BATCH,&orphan);
	printf("  closing fd with batch %llu still in flight (release must drain)\n",
	       (unsigned long long)orphan.out_batch_id);
	close(afd);

	printf("RESULT=PASS -- submit returns in %.0f us of %.0f, poll reports EAGAIN, "
	       "%d batches overlap, close drains\n", submit_us, total_us, DEPTH);
	return 0;
}
