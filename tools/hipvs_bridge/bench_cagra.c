#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/cuda_runtime.h>
#include <dlpack/dlpack.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec+ts.tv_nsec*1e-9; }
static void* read_file(const char* path, size_t bytes){ FILE* f=fopen(path,"rb"); if(!f){perror(path); exit(2);} void* p=malloc(bytes); if(!p){perror("malloc"); exit(2);} if(fread(p,1,bytes,f)!=bytes){fprintf(stderr,"short read %s\n",path); exit(2);} fclose(f); return p; }
#define CK(x) do{ cuvsError_t e=(x); if(e!=CUVS_SUCCESS){ fprintf(stderr,"cuvs err %s @%d: %s\n",#x,__LINE__,cuvsGetLastErrorText()); exit(3);} }while(0)

int main(int argc,char**argv){
  if(argc<13){ fprintf(stderr,"usage: %s base n dim query nq gt topk graph_degree intermediate niter itopk width\n",argv[0]); return 2; }
  const char* base_path=argv[1]; long n=atol(argv[2]); long dim=atol(argv[3]);
  const char* query_path=argv[4]; long nq=atol(argv[5]); const char* gt_path=argv[6]; long topk=atol(argv[7]);
  size_t gd=atol(argv[8]), igd=atol(argv[9]), niter=atol(argv[10]), itopk=atol(argv[11]), width=atol(argv[12]);
  float* base=(float*)read_file(base_path,(size_t)n*dim*sizeof(float));
  float* query=(float*)read_file(query_path,(size_t)nq*dim*sizeof(float));
  int32_t* gt=(int32_t*)read_file(gt_path,(size_t)nq*topk*sizeof(int32_t));

  double t0=now_s();
  cuvsResources_t res; CK(cuvsResourcesCreate(&res));
  cuvsCagraIndexParams_t ip; CK(cuvsCagraIndexParamsCreate(&ip));
  ip->build_algo=NN_DESCENT; ip->graph_degree=gd; ip->intermediate_graph_degree=igd; ip->nn_descent_niter=niter;
  cuvsCagraIndex_t idx; CK(cuvsCagraIndexCreate(&idx));
  DLManagedTensor ds; int64_t shape[2]={n,dim};
  ds.dl_tensor.data=base; ds.dl_tensor.device.device_type=kDLCPU; ds.dl_tensor.device.device_id=0;
  ds.dl_tensor.ndim=2; ds.dl_tensor.dtype.code=kDLFloat; ds.dl_tensor.dtype.bits=32; ds.dl_tensor.dtype.lanes=1;
  ds.dl_tensor.shape=shape; ds.dl_tensor.strides=NULL; ds.dl_tensor.byte_offset=0;
  CK(cuvsCagraBuild(res,ip,&ds,idx));
  double build_s=now_s()-t0;

  float* qd=NULL; uint32_t* nd=NULL; float* dd=NULL;
  CK(cuvsRMMAlloc(res,(void**)&qd,(size_t)nq*dim*sizeof(float)));
  CK(cuvsRMMAlloc(res,(void**)&nd,(size_t)nq*topk*sizeof(uint32_t)));
  CK(cuvsRMMAlloc(res,(void**)&dd,(size_t)nq*topk*sizeof(float)));
  cudaError_t ce1=cudaMemcpy(qd,query,(size_t)nq*dim*sizeof(float),cudaMemcpyDefault);
  if(ce1) fprintf(stderr,"q copy err %s\n",cudaGetErrorString(ce1));
  DLManagedTensor qt,nt,dt; int64_t qs[2]={nq,dim}, ns[2]={nq,topk}, ds2[2]={nq,topk};
  qt.dl_tensor.data=qd; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0; qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1; qt.dl_tensor.shape=qs; qt.dl_tensor.strides=NULL; qt.dl_tensor.byte_offset=0;
  nt=qt; nt.dl_tensor.data=nd; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=ns;
  dt=qt; dt.dl_tensor.data=dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=ds2;
  cuvsCagraSearchParams_t sp; CK(cuvsCagraSearchParamsCreate(&sp));
  sp->itopk_size=itopk; sp->search_width=width;
  cuvsFilter filter; filter.type=NO_FILTER; filter.addr=(uintptr_t)0;
  double s0=now_s();
  CK(cuvsCagraSearch(res,sp,idx,&qt,&nt,&dt,filter));
  double search_s=now_s()-s0;
  uint32_t* nh=(uint32_t*)malloc((size_t)nq*topk*sizeof(uint32_t));
  cudaError_t ce2=cudaMemcpy(nh,nd,(size_t)nq*topk*sizeof(uint32_t),cudaMemcpyDefault);
  if(ce2) fprintf(stderr,"n copy err %s\n",cudaGetErrorString(ce2));
  printf("q0 ids:"); for(long j=0;j<8;j++) printf(" %u",nh[j]);
  printf(" | q0 gt:"); for(long j=0;j<8;j++) printf(" %d",gt[j]); printf("\n");
  long hit10=0,tot10=0,hit100=0,tot100=0;
  for(long q=0;q<nq;q++){
    for(long k=0;k<10 && k<topk;k++){ int32_t g=gt[q*topk+k]; for(long j=0;j<topk;j++){ if((int32_t)nh[q*topk+j]==g){hit10++;break;} } tot10++; }
    for(long k=0;k<topk;k++){ int32_t g=gt[q*topk+k]; for(long j=0;j<topk;j++){ if((int32_t)nh[q*topk+j]==g){hit100++;break;} } tot100++; }
  }
  printf("RESULT n=%ld dim=%ld nq=%ld topk=%ld gd=%zu igd=%zu niter=%zu itopk=%zu width=%zu build_s=%.3f search_s=%.3f qps=%.1f recall10=%.4f recall100=%.4f\n",
    n,dim,nq,topk,gd,igd,niter,itopk,width,build_s,search_s,nq/search_s,(double)hit10/tot10,(double)hit100/tot100);
  return 0;
}
