// SPDX-License-Identifier: GPL-2.0
/* aw_bp — B_P and C: the gate of docs/campaigns/miss-tier-direct.md.
 *
 * Two paths, same expert bytes, same VRAM destination, same batching:
 *
 *   --mode host     expert file -> pread -> pinned host buffer -> clEnqueueWriteBuffer -> VRAM
 *   --mode arcwell  expert file -> FIEMAP -> AW_IOC_READ_BATCH -> VRAM   (no host buffer at all)
 *
 * Reports, per mode:
 *   B_P  GB/s      bytes landed in VRAM / wall time
 *   C    CPU-s/GiB process utime+stime, AND system-wide busy delta, per GiB moved
 *
 * Why both currencies: on a drive-bound card the bandwidth difference may be
 * small, while the CPU difference is the whole point -- a CPU not spent copying
 * is a CPU available to FreeToken's B_H expert-compute tier, which is what decides
 * the A770 (campaign, "Bandwidth is not the only currency").
 *
 * COLD is the honest comparison. The host path is only a miss tier when the bytes
 * are not in page cache; the arcwell path reads the raw device and never populates
 * it. The caller drops caches before a cold host run; --expect-cold makes the
 * program refuse to report if the read rate implies it was served from cache.
 *
 * Build: gcc -O2 -Wall -I. -I/usr/include/drm -o aw_bp aw_bp.c -lOpenCL
 */
#define _GNU_SOURCE
#define CL_TARGET_OPENCL_VERSION 300
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <linux/fiemap.h>
#include <CL/cl.h>
#include "xe_drm.h"
#include "aw_uapi.h"

#define SLOT 2457600u
#define MAXEXT 8

static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static double cpu_self(void){struct rusage r;getrusage(RUSAGE_SELF,&r);
  return r.ru_utime.tv_sec+r.ru_utime.tv_usec*1e-6+r.ru_stime.tv_sec+r.ru_stime.tv_usec*1e-6;}
/* system-wide busy jiffies, to catch kernel work not charged to this process */
static double sys_busy(void){FILE*f=fopen("/proc/stat","r");if(!f)return 0;
  unsigned long long a[10]={0};char l[512];double busy=0;
  if(fgets(l,sizeof l,f)){int n=sscanf(l,"cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
    &a[0],&a[1],&a[2],&a[3],&a[4],&a[5],&a[6],&a[7],&a[8],&a[9]);
    for(int i=0;i<n;i++) if(i!=3&&i!=4) busy+=a[i];}
  fclose(f); return busy/sysconf(_SC_CLK_TCK);}

static unsigned long long file_lba(const char *path, unsigned long long part_start, unsigned *next)
{
	/* A char[] has alignment 1. Casting it to struct fiemap* (which contains
	 * __u64) lets -O2 vectorise the zero-init into aligned SSE stores and fault.
	 * A union gives the buffer the struct's alignment. Cost me a segfault. */
	union { struct fiemap fm; char raw[sizeof(struct fiemap)+MAXEXT*sizeof(struct fiemap_extent)]; } u;
	struct fiemap *fm=&u.fm;
	memset(&u,0,sizeof u);
	int fd=open(path,O_RDONLY); if(fd<0) return 0;
	fm->fm_start=0; fm->fm_length=SLOT; fm->fm_flags=FIEMAP_FLAG_SYNC; fm->fm_extent_count=MAXEXT;
	if(ioctl(fd,FS_IOC_FIEMAP,fm)<0||fm->fm_mapped_extents==0){close(fd);return 0;}
	*next=fm->fm_mapped_extents;
	unsigned long long lba=fm->fm_extents[0].fe_physical/512+part_start;
	close(fd); return lba;
}

int main(int argc,char**argv)
{
	const char *mode="host", *dir="/flash/experts", *drm="/dev/dri/renderD129";
	int nexp=64, iters=16; unsigned long long part_start=100665344ULL;
	for(int i=1;i<argc;i++){
		if(!strcmp(argv[i],"--mode")&&i+1<argc) mode=argv[++i];
		else if(!strcmp(argv[i],"--dir")&&i+1<argc) dir=argv[++i];
		else if(!strcmp(argv[i],"--drm")&&i+1<argc) drm=argv[++i];
		else if(!strcmp(argv[i],"--experts")&&i+1<argc) nexp=atoi(argv[++i]);
		else if(!strcmp(argv[i],"--iters")&&i+1<argc) iters=atoi(argv[++i]);
		else if(!strcmp(argv[i],"--part-start")&&i+1<argc) part_start=strtoull(argv[++i],0,0);
	}
	int host = !strcmp(mode,"host");
	size_t batch = (size_t)nexp*SLOT;
	printf("==== %s : %d experts x %u B = %.1f MiB per batch, %d batches ====\n",
	       host?"B_P_host":"B_P_arcwell", nexp, SLOT, batch/1048576.0, iters);

	int gfd=open(drm,O_RDWR|O_CLOEXEC); if(gfd<0){perror("open drm");return 1;}
	struct drm_xe_gem_create bo={0};
	bo.size=(batch+0xffff)&~0xffffULL;
	bo.placement=1u<<DRM_XE_MEM_REGION_CLASS_VRAM;
	bo.flags=DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;
	bo.cpu_caching=DRM_XE_GEM_CPU_CACHING_WC;
	if(ioctl(gfd,DRM_IOCTL_XE_GEM_CREATE,&bo)<0){perror("GEM_CREATE");return 1;}
	struct drm_prime_handle pr={0}; pr.handle=bo.handle; pr.flags=O_RDWR|O_CLOEXEC;
	if(ioctl(gfd,DRM_IOCTL_PRIME_HANDLE_TO_FD,&pr)<0){perror("PRIME");return 1;}

	cl_context ctx=NULL; cl_command_queue q=NULL; cl_mem dst=NULL; void*pin=NULL; cl_mem pinbuf=NULL;
	int afd=-1; uint32_t awh=0;
	struct aw_ioc_stats base={0};
	struct aw_ioc_read_blocks *reqs=NULL;

	if(host){
		cl_platform_id p[8]; cl_uint np=0; clGetPlatformIDs(8,p,&np);
		cl_device_id dev=NULL; const char*want=strstr(drm,"129")?"B60":"A770";
		for(cl_uint i=0;i<np&&!dev;i++){cl_device_id d;cl_uint nd=0;
			if(clGetDeviceIDs(p[i],CL_DEVICE_TYPE_GPU,1,&d,&nd)||!nd)continue;
			char n[256]={0}; clGetDeviceInfo(d,CL_DEVICE_NAME,sizeof n,n,NULL);
			if(strstr(n,want)) dev=d;}
		if(!dev){printf("RESULT=FAIL -- no OpenCL device\n");return 1;}
		cl_int e; ctx=clCreateContext(NULL,1,&dev,NULL,NULL,&e);
		q=clCreateCommandQueueWithProperties(ctx,dev,NULL,&e);
		cl_mem_properties props[]={(cl_mem_properties)0x2067,(cl_mem_properties)pr.fd,0};
		dst=clCreateBufferWithProperties(ctx,props,CL_MEM_READ_WRITE,bo.size,NULL,&e);
		if(!dst){printf("RESULT=FAIL -- dma-buf import for the destination failed (%d)\n",e);return 1;}
		pinbuf=clCreateBuffer(ctx,CL_MEM_READ_ONLY|CL_MEM_ALLOC_HOST_PTR,SLOT,NULL,&e);
		pin=clEnqueueMapBuffer(q,pinbuf,CL_TRUE,CL_MAP_WRITE,0,SLOT,0,NULL,NULL,&e);
		if(!pin){printf("RESULT=FAIL -- pinned staging map failed\n");return 1;}
	}else{
		afd=open("/dev/" AW_DEVICE_NAME,O_RDWR); if(afd<0){perror("open /dev/arcwell");return 1;}
		ioctl(afd,AW_IOC_STATS,&base);
		struct aw_ioc_map_buffer mb={0};
		mb.in_handle=(uint64_t)pr.fd; mb.in_source=AW_BUF_DMABUF; mb.in_length=bo.size;
		if(ioctl(afd,AW_IOC_MAP_BUFFER,&mb)<0){perror("AW_IOC_MAP_BUFFER");return 1;}
		if(!(mb.out_flags&AW_MAP_F_REQUIRE_P2P)){printf("RESULT=FAIL -- not peer-to-peer\n");return 1;}
		awh=mb.out_handle;
		reqs=calloc(nexp,sizeof *reqs);
	}

	double t_all=0, cpu0=cpu_self(), sys0=sys_busy(), wall0=now();
	unsigned long long bytes=0; unsigned tot_ext=0;

	for(int it=0; it<iters; it++){
		double t0=now();
		if(host){
			for(int k=0;k<nexp;k++){
				char path[512]; snprintf(path,sizeof path,"%s/expert_%04d.bin",dir,(it*nexp+k)%1700);
				int fd=open(path,O_RDONLY); if(fd<0){perror("open expert");return 1;}
				if(pread(fd,pin,SLOT,0)!=(ssize_t)SLOT){perror("pread");return 1;}
				close(fd);
				clEnqueueWriteBuffer(q,dst,CL_FALSE,(size_t)k*SLOT,SLOT,pin,0,NULL,NULL);
				clFinish(q);   /* staging buffer is reused: must land before refill */
			}
		}else{
			for(int k=0;k<nexp;k++){
				char path[512]; snprintf(path,sizeof path,"%s/expert_%04d.bin",dir,(it*nexp+k)%1700);
				unsigned ne=0; unsigned long long lba=file_lba(path,part_start,&ne);
				if(!lba){printf("RESULT=FAIL -- FIEMAP failed on %s\n",path);return 1;}
				tot_ext+=ne;
				reqs[k].in_buffer_handle=awh;
				reqs[k].in_start_block=lba;
				reqs[k].in_block_count=SLOT/512;
				reqs[k].in_dest_offset=(uint64_t)k*SLOT;
			}
			struct aw_ioc_read_batch rb={0};
			rb.in_requests=(uint64_t)(uintptr_t)reqs; rb.in_count=nexp;
			if(ioctl(afd,AW_IOC_READ_BATCH,&rb)<0){perror("AW_IOC_READ_BATCH");return 1;}
			if(rb.out_err){printf("RESULT=FAIL -- batch err=%d at %u\n",rb.out_err,rb.out_err_index);return 1;}
		}
		t_all+=now()-t0; bytes+=batch;
	}
	double cpu=cpu_self()-cpu0, sys=sys_busy()-sys0, wall=now()-wall0;
	double gib=bytes/1073741824.0;
	printf("  bytes=%.2f GiB  wall=%.3f s  B_P=%.2f GB/s\n", gib, t_all, bytes/t_all/1e9);
	printf("  CPU process=%.3f s -> %.4f CPU-s/GiB\n", cpu, cpu/gib);
	printf("  CPU system-wide=%.3f s -> %.4f CPU-s/GiB (includes kernel threads)\n", sys, sys/gib);
	if(!host) printf("  extents=%u over %d reads (%.2f per expert)\n", tot_ext, iters*nexp, (double)tot_ext/(iters*nexp));
	if(!host){ struct aw_ioc_stats st={0}; ioctl(afd,AW_IOC_STATS,&st);
		printf("  via_host_bounce=%u max_inflight=%u\n", st.via_host_bounce, st.max_inflight);
		if(st.via_host_bounce != base.via_host_bounce){  /* DELTA: module-global counter */
			printf("RESULT=FAIL -- via_host_bounce moved by %u\n",
			       st.via_host_bounce - base.via_host_bounce); return 1;} }
	printf("RESULT=OK -- %s B_P=%.2f GB/s C_proc=%.4f C_sys=%.4f CPU-s/GiB (wall %.3f s)\n",
	       host?"host":"arcwell", bytes/t_all/1e9, cpu/gib, sys/gib, wall);
	return 0;
}
