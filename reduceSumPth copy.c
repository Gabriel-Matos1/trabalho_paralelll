#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sys/time.h>
#include <stdlib.h>


#include "chrono.c"



#define DEBUG 0
//#define DEBUG 1
#define MAX_THREADS 64
#define LOOP_COUNT 1


#define FLOAT 1
#define DOUBLE 2

#define TYPE FLOAT // CHOICE OF FLOAT or DOUBLE

#if TYPE == FLOAT
   #define element_TYPE float
#elif TYPE == DOUBLE   
   #define element_TYPE double
#endif   


#if TYPE == FLOAT
   #define MAX_TOTAL_ELEMENTS (500*1000*1000)  // if each float takes 4 bytes
                                            // will have a maximum 500 million FLOAT elements
                                            // which fits in 2 GB of RAM
#elif TYPE == DOUBLE   
   #define MAX_TOTAL_ELEMENTS (250*1000*1000)  // if each float takes 4 bytes
                                            // will have a maximum 250 million DOUBLE elements
                                            // which fits in 2 GB of RAM
#endif   



//** FUNÇÕES DESCARTADAS

/*
int min( int a, int b )
{
   if( a < b )
      return a;
   else
      return b;
}

element_TYPE plus( element_TYPE a, element_TYPE b )
{
    return a + b;
}
*/


//** FUNÇÕES DADAS PELO PROFESSOR */

/* ------------------------------------------------------------------ */
/* verify_histogram: three-stage correctness check                     */
/*                                                                      */
/* Stage 1: hist_1thr[b] == hist_nthr[b] for all b.                   */
/* Stage 2: (only if stage 1 passes) serial recount of hist_nthr       */
/*          using a simple linear scan — independent of find_bin and   */
/*          the parallel pool. Reports any bin with wrong count.       */
/* Stage 3: (only if stages 1+2 pass) sum of all bins == nelements.   */
/*                                                                      */
/* Returns 1 if ALL three stages pass, 0 otherwise.                   */
/* ------------------------------------------------------------------ */
static int verify_histogram(
    const long long *data,
    long long        nelements,
    const long long *limits,
    int              nbins,
    const long long *hist_1thr,
    const long long *hist_nthr)
{
    /* Stage 1: 1-thread vs N-thread */
    int s1_ok = 1;
    for (int b = 0; b < nbins; b++) {
        if (hist_1thr[b] != hist_nthr[b]) {
            fprintf(stderr,
                    "  VERIFY FAIL stage1: bin %d  1thr=%lld  Nthr=%lld\n",
                    b, hist_1thr[b], hist_nthr[b]);
            s1_ok = 0;
        }
    }
    if (!s1_ok) return 0;

    /* Stage 2: serial recount vs hist_nthr
     * Uses a simple linear scan: walk limits[] left to right.
     * Completely independent of find_bin and the thread pool.         */
    long long *recount = (long long *)calloc(nbins, sizeof(long long));
    if (!recount) { perror("calloc recount"); return 0; }

    for (long long i = 0; i < nelements; i++) {
        long long v = data[i];
        int b = 0;
        /* Linear scan: find rightmost bin whose left edge <= v */
        while (b < nbins - 1 && v >= limits[b + 1]) b++;
        recount[b]++;
    }

    int s2_ok = 1;
    for (int b = 0; b < nbins; b++) {
        if (recount[b] != hist_nthr[b]) {
            fprintf(stderr,
                    "  VERIFY FAIL stage2: bin %d  recount=%lld  Nthr=%lld\n",
                    b, recount[b], hist_nthr[b]);
            s2_ok = 0;
        }
    }
    free(recount);
    if (!s2_ok) return 0;

    /* Stage 3: invariant — sum of bins == nelements */
    long long total = 0;
    for (int b = 0; b < nbins; b++) total += hist_nthr[b];
    if (total != nelements) {
        fprintf(stderr,
                "  VERIFY FAIL stage3: sum=%lld expected=%lld\n",
                total, nelements);
        return 0;
    }

    return 1;   /* all stages passed */
}









static void gen_test_data_balanced2(long long *data,
                                     long long nelements,
                                     int nbins)
{
    if (nelements <= 0 || nbins <= 0) return;

    /* 1. Fill with 0, 1, 2, ..., nelements-1 */
    for (long long i = 0; i < nelements; i++)
        data[i] = i;

    /* 2. Fisher-Yates shuffle */
    for (long long i = nelements - 1; i > 0; i--) {
        long long j = (long long)(rand63() % (unsigned long long)(i + 1));
        ll_swap(&data[i], &data[j]);
    }

    /* 3. Change each value of the input to be in [0, nbins) */
    for (long long i = 0; i < nelements; i++)
        data[i] %= nbins;
}

    
/*
 * rand63: Returns a uniform random unsigned 64-bit integer
 *         in the range [0, 2^63 - 1] using 63 random bits.
 *
 * rand() is guaranteed to produce at least 15 random bits.
 * We extract up to 21 bits per call (3 × 21 = 63 bits).
 * This gives excellent uniformity for generating numbers in [0, stride)
 * where stride <= LLONG_MAX (i.e. any positive long long).
 *
 * Recommended usage for uniform random value in [0, stride):
 * given that stride is long long
 *
 * Use: 
 *      long long jitter = (long long)(rand63() % (unsigned long long)stride);
 * to get a uniform random in [0, stride) annd assign to jitter. 
 */
static inline unsigned long long rand63(void)
{
    return (  (unsigned long long)(unsigned)rand()        )
           | ( (unsigned long long)(unsigned)rand() << 21 )
           | ( (unsigned long long)(unsigned)rand() << 42 );
}
   
    
/*
 * Generate a uniformly distributed random signed 64-bit integer
 * (full range of long long, including negative values).
 *
 * rand() is required to return at least 15 random bits.
 * We use five calls:
 *   - 4 calls × 15 bits = 60 bits
 *   - 1 final call masked to 4 bits
 * Total: 64 random bits.
 *
 * All operations are done in unsigned long long to avoid
 * undefined behaviour from signed integer overflow. The resulting
 * bit pattern is later reinterpreted as signed long long.
 */
static inline long long rand64(void)
{
    union {
        unsigned long long u;
        long long          s;
    } x;

    x.u = ((unsigned long long)(unsigned)rand() << 49)
        | ((unsigned long long)(unsigned)rand() << 34)
        | ((unsigned long long)(unsigned)rand() << 19)
        | ((unsigned long long)(unsigned)rand() <<  4)
        | ((unsigned long long)(unsigned)rand() & 0xF);

    return x.s;
}
/* ------------------------------------------------------------------ */
/* build_limits: jittered-pivot construction (SP2)                    */
/*                                                                      */
/*   stride    = n / npivots                                           */
/*   jitter_i is a random number generated in interval [0, stride)    */
/*       uses rand63() to generate a random jitter per pivot           */
/*   Pivots[i] = data[i*stride + jitter_i]                            */
/*                                                                      */
/* Option A: limits[0]=LLONG_MIN and limits[nbins]=LLONG_MAX guarantee */
/* every long long value is classified correctly with no min/max scan. */
/* ------------------------------------------------------------------ */

static void build_limits_sp2_serial(const long long *Input, 
                                    long long n,
                                    int npivots, 
                                    int nbins,
                                    long long *pivots, 
                                    long long *limits )

















pthread_t parallelReduce_Thread[ MAX_THREADS ];
int parallelReduce_thread_id[ MAX_THREADS ];
element_TYPE parallelReduce_partialSum[ MAX_THREADS ];   

int parallelReduce_nThreads;  // numero efetivo de threads
               // obtido da linha de comando  
int parallelReduce_nTotalElements;  // numero total de elementos
               // obtido da linha de comando      
               
element_TYPE InputVector[ MAX_TOTAL_ELEMENTS ];   // will NOT use malloc
                                     // for simplicity                              
element_TYPE *InVec = InputVector;
        
pthread_barrier_t parallelReduce_barrier;


void *reducePartialSum(void *ptr)
{
    int myIndex = *((int *)ptr);
    //int nElements = parallelReduce_nTotalElements / parallelReduce_nThreads;
    int nElements = (parallelReduce_nTotalElements+(parallelReduce_nThreads-1))
                    / parallelReduce_nThreads;
        
    // assume que temos pelo menos 1 elemento por thhread
    int first = myIndex * nElements;
    int last = min( (myIndex+1) * nElements, parallelReduce_nTotalElements ) - 1;

    #if DEBUG == 1
      printf("thread %d here! first=%d last=%d nElements=%d\n", 
                                  myIndex, first, last, nElements );
    #endif
    
//    if( myIndex != 0 )
    while( true ) {
    
       // pergunta: seria possivel usar somente uma barreira por vez ?
    
        // all worker threads will be waiting here for the caller thread
        pthread_barrier_wait( &parallelReduce_barrier );    
        
       // work with my chunck
       register element_TYPE myPartialSum = 0;

       for( int i=first; i<=last ; i++ )
           //myPartialSum = plus( myPartialSum, InputVector[i] );
           myPartialSum += InVec[i];

       // store my result 
       parallelReduce_partialSum[ myIndex ] = myPartialSum;     
        
       pthread_barrier_wait( &parallelReduce_barrier );    
       if( myIndex == 0 )
          return NULL;           // return to caller thread
          
    }
    
    // NEVER HERE!
    if( myIndex != 0 )
          pthread_exit( NULL );
          
    return NULL;      
}


element_TYPE parallel_reduceSum( element_TYPE InputVec[], 
                                 int nTotalElements, int nThreads )
{

    static int initialized = 0;
    parallelReduce_nTotalElements = nTotalElements;
    parallelReduce_nThreads = nThreads;
    
    InVec = InputVec;
    
    if( ! initialized ) { 
       pthread_barrier_init( &parallelReduce_barrier, NULL, nThreads );
       // thread 0 will be the caller thread
    
       // cria todas as outra threds trabalhadoras
       parallelReduce_thread_id[0] = 0;
       for( int i=1; i < nThreads; i++ ) {
         parallelReduce_thread_id[i] = i;
         pthread_create( &parallelReduce_Thread[i], NULL, 
                      reducePartialSum, &parallelReduce_thread_id[i]);
       }

       initialized = 1;
    }

    // above, int this version, all other worker threads from 1 to nThreads will 
    //   start working imediatelly (no barriers to start working)
    
    // caller thread will be thread 0, and will start working on its chunk
    reducePartialSum( &parallelReduce_thread_id[0] ); 
        
    // chegando aqui todas as threads sincronizaram, 
    //  na barreira no final da funçao reducePartialSum (até a 0)
    //  entao o vertor de somasPartcias estah pronto
    
    // a thread chamadora faz, entao, a reduçao da soma global
    element_TYPE globalSum = 0;
    for( int i=0; i<nThreads ; i++ ) {
        //printf( "globalSum = %f\n", globalSum );
        globalSum += parallelReduce_partialSum[i];
    }    
    
    // isso é necessário ?
    //pthread_barrier_destroy( &myBarrier );
    
    // obs: como as threads trabalhadoras sincronizaram e irão terminar,
    //      não é necessário esperar o término delas
    
    return globalSum;
}

int main( int argc, char *argv[] )
{
    int i;
    int nThreads;
    int nTotalElements;
    
    chronometer_t parallelReductionTime;
    
    if( argc != 3 ) {
         printf( "usage: %s <nTotalElements> <nThreads>\n" ,
                 argv[0] ); 
         return 0;
    } else {
         nThreads = atoi( argv[2] );
         if( nThreads == 0 ) {
              printf( "usage: %s <nTotalElements> <nThreads>\n" ,
                 argv[0] );
              printf( "<nThreads> can't be 0\n" );
              return 0;
         }     
         if( nThreads > MAX_THREADS ) {  
              printf( "usage: %s <nTotalElements> <nThreads>\n" ,
                 argv[0] );
              printf( "<nThreads> must be less than %d\n", MAX_THREADS );
              return 0;
         }     
         nTotalElements = atoi( argv[1] ); 
         if( nTotalElements > MAX_TOTAL_ELEMENTS ) {  
              printf( "usage: %s <nTotalElements> <nThreads>\n" ,
                 argv[0] );
              printf( "<nTotalElements> must be up to %d\n", MAX_TOTAL_ELEMENTS );
              return 0;
         }     
    }
    
    
    #if TYPE == FLOAT
        printf( "will use %d threads to reduce %d total FLOAT elements\n\n", nThreads, nTotalElements );
    #elif TYPE == DOUBLE   
        printf( "will use %d threads to reduce %d total DOUBLE elements\n\n", nThreads, nTotalElements );
    #endif   
    
    // inicializaçoes
    // initialize InputVector
//    for( int i=0; i<nTotalElements ; i++ )
    for( int i=0; i<MAX_TOTAL_ELEMENTS ; i++ )
        InputVector[i] = (element_TYPE)1;
        
    chrono_reset( &parallelReductionTime );
    chrono_start( &parallelReductionTime );

      // call it N times
      #define NTIMES 10
      printf( "will call parallel_reduceSum %d times\n", NTIMES );
            
      element_TYPE globalSum;
      int start_position = 0;
      InVec = &InputVector[start_position];

      for( int i=0; i<NTIMES ; i++ ) {
           //globalSum = parallel_reduceSum( InputVector,
           //                                nTotalElements, nThreads );
           
           globalSum = parallel_reduceSum( InVec,
                                           nTotalElements, nThreads );
           // garante que na proxima rodada todos os elementos estarão FORA do cache
           start_position += nTotalElements;
           // volta ao inicio do vetor 
           //   SE nao cabem nTotalElements a partir de start_position
           if( (start_position + nTotalElements) > MAX_TOTAL_ELEMENTS )
              start_position = 0;
           InVec = &InputVector[start_position];  

           // cacheflush(void *addr, int nbytes, int cache); 
           //int r = cacheflush( InputVector, nTotalElements*sizeof(element_TYPE), DCACHE );     
           //if( r )
           //   fprintf( stderr, "cache NOT flushed\n" );
              
          //printf( "address = %p \n", InputVector ); /* guaranteed to be aligned within a single cache line */
          // myCacheFlush( unsigned char *addr, int nBytes )
          //myCacheFlush( (unsigned char *)InputVector, nTotalElements*sizeof(element_TYPE ) );
          
                                        
           // wait 50 us == 50000 ns
           //nanosleep((const struct timespec[]){{0, 50000L}}, NULL);                                
      }     
                                           
    // Measuring time after parallel_reduceSum finished...
    chrono_stop( &parallelReductionTime );

    // main imprime o resultado global
    #if TYPE == FLOAT
       printf( "globalSum = %f\n", globalSum );
    #elif TYPE == DOUBLE
       printf( "globalSum = %lf\n", globalSum );
    #endif
    
    chrono_reportTime( &parallelReductionTime, "parallelReductionTime" );
    
    // calcular e imprimir a VAZAO (numero de operacoes/s)
    double total_time_in_seconds = (double) chrono_gettotal( &parallelReductionTime ) /
                                      ((double)1000*1000*1000);
    printf( "total_time_in_seconds: %lf s\n", total_time_in_seconds );
                                  
    double OPS = ((double)nTotalElements*NTIMES)/total_time_in_seconds;
    printf( "Throughput: %lf OP/s\n", OPS );
    
    return 0;
}
