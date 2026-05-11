#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>

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
                                    long long *limits );
long long *randomizador(long long n_elementos, int tb2, int numero_faixas_hist){
    
    long long *vetor_final = (long long*) malloc( n_elementos * sizeof(long long));
    
    if(vetor_final == NULL){
        fprintf(stderr, "erro ao alocar memória para vetor_final");
        exit(1);
    }

    for(long long i=0; i<n_elementos; i++){
        vetor_final[i] = rand64();
    }

    if(tb2 == 1){
            gen_test_data_balanced2(vetor_final, n_elementos, numero_faixas_hist);
    }

    return vetor_final;

}

//./parallel-histo 16000000 1000 32 8 10 -tb2
int main( int argc, char *argv[] )
{
    long long numero_elementos = argv[1];
    int numero_separadores_pivot = argv[2];
    int numero_faixas_histograma = argv[3];
    int numero_threads = argv[4];
    int numero_rodadas_de_execução = argv[5];
    int tb2=0;
    if(argv[6] == '-tb2'){
        tb2=1;
    }
    long long *vetor_rand = randomizador(numero_elementos, tb2, numero_faixas_histograma);



    return 0;
}
