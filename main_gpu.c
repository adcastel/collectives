//
// Created by Adrián Castello Gimeno on 04/01/2021.
//

#include <stdio.h>
#include "cuda_runtime_api.h"
#include "nccl.h"
#include "mpi.h"
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>


#define MPICHECK(cmd) do {                          \
  int e = cmd;                                      \
  if( e != MPI_SUCCESS ) {                          \
    printf("Failed: MPI error %s:%d '%d'\n",        \
        __FILE__,__LINE__, e);   \
    exit(EXIT_FAILURE);                             \
  }                                                 \
} while(0)


#define CUDACHECK(cmd) do {                         \
  cudaError_t e = cmd;                              \
  if( e != cudaSuccess ) {                          \
    printf("Failed: Cuda error %s:%d '%s'\n",             \
        __FILE__,__LINE__,cudaGetErrorString(e));   \
    exit(EXIT_FAILURE);                             \
  }                                                 \
} while(0)


#define NCCLCHECK(cmd) do {                         \
  ncclResult_t r = cmd;                             \
  if (r!= ncclSuccess) {                            \
    printf("Failed, NCCL error %s:%d '%s'\n",             \
        __FILE__,__LINE__,ncclGetErrorString(r));   \
    exit(EXIT_FAILURE);                             \
  }                                                 \
} while(0)


static uint64_t getHostHash(const char* string) {
    // Based on DJB2, result = result * 33 + char
    uint64_t result = 5381;
    for (int c = 0; string[c] != '\0'; c++){
        result = ((result << 5) + result) + string[c];
    }
    return result;
}


static void getHostName(char* hostname, int maxlen) {
    gethostname(hostname, maxlen);
    for (int i=0; i< maxlen; i++) {
        if (hostname[i] == '.') {
            hostname[i] = '\0';
            return;
        }
    }
}

#define START_TEST      double time,time2;\
			double time_all = 0.0;\
			int r;\
			for (r = 0; r < reps; r++){\
		        init(sendbuff,hsendbuff, recvbuff, hrecvbuff, sol, size, nRanks);\
		        time = MPI_Wtime();\

#define END_TEST	time = MPI_Wtime() - time;\
        		MPI_Barrier(MPI_COMM_WORLD);\
        		MPI_Reduce(&time,&time2,1,MPI_DOUBLE,MPI_MAX,0,MPI_COMM_WORLD);\
                        if(myRank == 0){ time = time2; printf("%f\n",time); }\
        		/*check(sendbuff,hsendbuff,hrecvbuff,recvbuff,size,sol,comm, myRank);*/\
        		time_all+=time;\
			}\
    			return time_all/reps;

void init(float * sendbuff, float * hsendbuff, float * recvbuff, float * hrecvbuff, float * sol, size_t size, int nRanks){

    int i=0;
    for(i=0;i<size;i++){
        hsendbuff[i]= i * 1.0f;
        hrecvbuff[i]= 0.0f;
        sol[i]= i* 1.0f * nRanks;
    }

    CUDACHECK(cudaMemcpy(sendbuff, hsendbuff, size * sizeof(float), cudaMemcpyHostToDevice));
    CUDACHECK(cudaMemcpy(recvbuff, hrecvbuff, size * sizeof(float), cudaMemcpyHostToDevice));
}

int myreturn(float * sendbuff, float * recvbuff, float * hsendbuff, float * hrecvbuff, float * sol,
             ncclComm_t comm, int ret){

    CUDACHECK(cudaFree(sendbuff));
    CUDACHECK(cudaFree(recvbuff));
    free(hsendbuff);
    free(hrecvbuff);
    free(sol);
    ncclCommDestroy(comm);
    MPICHECK(MPI_Finalize());
    return ret;

}

void check(float * sendbuff, float * hsendbuff, float * hrecvbuff, float * recvbuff, size_t size, float * sol, ncclComm_t comm, int myRank){

    CUDACHECK(cudaMemcpy(hrecvbuff, recvbuff, size * sizeof(float), cudaMemcpyDeviceToHost));
    int i;
    for(i=0;i<size;i++){
        if(sol[i] != hrecvbuff[i]){
            printf("[MPI Rank %d] Error at element %d. Expcted %f, value %f\n", myRank,i, sol[i], hrecvbuff[i]);
            int r = myreturn(sendbuff, recvbuff, hsendbuff, hrecvbuff, sol, comm, 1);
            exit;
        }
    }
}

double ori_nccl_allreduce(float * sendbuff, float * recvbuff, float * hsendbuff, float * hrecvbuff,
                          size_t size, float * sol, ncclComm_t comm, cudaStream_t * s, int myRank, int nRanks, int reps){
    //communicating using NCCL
    START_TEST
    NCCLCHECK(ncclAllReduce((const void*)sendbuff, (void*)recvbuff, size, ncclFloat, ncclSum,
                            comm, s[0]));
    //completing NCCL operation by synchronizing on the CUDA stream
    CUDACHECK(cudaStreamSynchronize(s[0]));
    END_TEST
}


double part_nccl_allreduce(float * sendbuff, float * recvbuff, float * hsendbuff, float * hrecvbuff,
                          size_t size, float * sol, ncclComm_t comm, cudaStream_t * st, int numstreams, int myRank, int nRanks, int reps, int halfs){
    //communicating using NCCL
 
    if(size < halfs){halfs=1;}
    if(numstreams > halfs) numstreams = halfs;
    size_t half_size = size/halfs;
    int h, s;
    START_TEST;
    size_t sent = 0;
    for(h=0;h<halfs-1;h++)
    {
        NCCLCHECK(ncclAllReduce((const void*)&sendbuff[h*half_size], (void*)&recvbuff[h*half_size], half_size, ncclFloat, ncclSum,
                            comm, st[h%numstreams]));
        sent+=half_size;
  //   if(myRank==0) printf("Allreduce %d of size %d (total:%d)\n",h,half_size,sent);
    }
    h=halfs-1;
    NCCLCHECK(ncclAllReduce((const void*)&sendbuff[h*half_size], (void*)&recvbuff[h*half_size], size-sent, ncclFloat, ncclSum,
              comm, st[h%numstreams]));
//     if(myRank==0) printf("Allreduce %d of size %d (total:%d)\n",h,size-sent, size);
    //completing NCCL operation by synchronizing on the CUDA stream
    for (s=0; s<numstreams; s++){
         CUDACHECK(cudaStreamSynchronize(st[s]));
    }
    END_TEST
}


int main(int argc, char* argv[])
{
    size_t size = 134217728*2; // 1 GB

    int myRank, nRanks, localRank = 0;
    int reps = 100;

    //initializing MPI
    MPICHECK(MPI_Init(&argc, &argv));
    MPICHECK(MPI_Comm_rank(MPI_COMM_WORLD, &myRank));
    MPICHECK(MPI_Comm_size(MPI_COMM_WORLD, &nRanks));


    //calculating localRank based on hostname which is used in selecting a GPU
/*    uint64_t hostHashs[nRanks];
    char hostname[1024];
    getHostName(hostname, 1024);
    hostHashs[myRank] = getHostHash(hostname);
    MPICHECK(MPI_Allgather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, hostHashs, sizeof(uint64_t), MPI_BYTE, MPI_COMM_WORLD));
    for (int p=0; p<nRanks; p++) {
        if (p == myRank) break;
        if (hostHashs[p] == hostHashs[myRank]) localRank++;
    }
*/
    // Usage: main_gpu [ori [parts [streams]]]
    // ori: 0|1 indicates if the original allreduce must be done (default 1)
    // parts: >=0 indicates the number of allreduce divisions (default 0)
    // streams: 0<x<=parts indicates the number of streams (default 1)
    int ori, parts, streams;

    ori = (argc > 1) ? atoi(argv[1]) : 1;
    parts = (argc > 2) ? atoi(argv[2]) : 0;
    streams = (argc > 3) ? atoi(argv[3]) : 1;
    if (parts > 0 && streams > parts) streams = parts;
    int chunk = (argc > 4) ? atoi(argv[4]): 0;
    int chunksize = (argc > 5) ? atoi(argv[5]): 262144; //262144 ints = 1MB
    size_t range_s = (argc > 6) ? atol(argv[6]): 0;
    size_t range_end = (argc > 7) ? atol(argv[7]): 0;


    ncclUniqueId id;
    ncclComm_t comm;
    float *sendbuff, *recvbuff;
    float *hsendbuff, *hrecvbuff;
    float *sol;
    cudaStream_t * st = malloc(streams*sizeof(cudaStream_t));

    char processor_name[MPI_MAX_PROCESSOR_NAME];
    int name_len;
    MPI_Get_processor_name(processor_name, &name_len);

    // Print off a hello world message so we ensure the correct MPI mapping (one per node)
    printf("#ANTES Hello world from processor %s, rank %d out of %d processors. GPU %d in communicator %d\n",
           processor_name, myRank, nRanks, localRank , id);

    //get NCCL unique ID at rank 0 and broadcast it to all others
    if (myRank == 0) ncclGetUniqueId(&id);
    MPICHECK(MPI_Bcast((void *)&id, sizeof(id), MPI_BYTE, 0, MPI_COMM_WORLD));

    // Print off a hello world message so we ensure the correct MPI mapping (one per node)
    printf("#Hello world from processor %s, rank %d out of %d processors. GPU %d in communicator %d\n",
           processor_name, myRank, nRanks, localRank , id);

    //picking a GPU based on localRank, allocate device buffers
    CUDACHECK(cudaSetDevice(localRank));
    CUDACHECK(cudaMalloc((void *)&sendbuff, size * sizeof(float)));
    CUDACHECK(cudaMalloc((void *)&recvbuff, size * sizeof(float)));
    for(int i = 0; i < streams; i++){
        CUDACHECK(cudaStreamCreate(&st[i]));
    }

    hsendbuff = malloc(size*sizeof(float));
    hrecvbuff = malloc(size*sizeof(float));
    sol = malloc(size*sizeof(float));

    //initializing NCCL
    NCCLCHECK(ncclCommInitRank(&comm, nRanks, id, myRank));

    if(myRank == 0){
        // Print a summary of the test
        printf("#Test with %d proceses\n",nRanks);
        printf("#Segmentation: %sabled with %d parts and %d streams\n",(parts == 0)? "dis" : "en", parts, streams);
        printf("#Chunk nccl: %sabled with chunksize of %d (elems) %d~MB and %d streams\n",
               (chunk == 0)? "dis" : "en", chunksize, chunksize*sizeof(float)/1024/1024, streams);
    }

    if(myRank == 0){
        // Prepare the output header
        printf("#SIZE(bytes)\t");
        if(ori == 1){
            printf("nccl_allreduce(%d)\t",nRanks);
        }

        if(parts){
            printf("%d_nccl_allreduce(%d)_in_%d\t",parts,nRanks, streams);
        }
        if(chunk == 1){
            printf("%d_chunk_nccl_allreduce(%d)_in_%d\t",chunksize,nRanks,streams);
        }
        printf("\n");
    }
        size_t init_count=1;
        size_t end_count = size;
    for (size_t s=init_count; s<=end_count; s*=2) {

        if (myRank == 0)
            printf("%lu\t\t", s * sizeof(float));
        if (ori) {
            double ori_time = ori_nccl_allreduce(sendbuff, recvbuff, hsendbuff, hrecvbuff, s, sol, comm, st, myRank,
                                                 nRanks, reps);
            if (myRank == 0) printf("%f\t", ori_time);
        }
        if(parts){
           double part_time = part_nccl_allreduce(sendbuff, recvbuff, hsendbuff, hrecvbuff,
                          s, sol, comm, st, streams, myRank, nRanks, reps, parts);
            if (myRank == 0) printf("%f\t", part_time);
        
        }
        if (myRank == 0) printf("\n");
    }

    printf("[MPI Rank %d] Success \n", myRank);
    return myreturn(sendbuff, recvbuff, hsendbuff, hrecvbuff, sol, comm, 0);
}
