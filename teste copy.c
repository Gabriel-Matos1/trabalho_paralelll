#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
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
#define LLC_SIZE (12ULL * 1024 * 1024)
#define EVICT_SIZE (3ULL * LLC_SIZE)

static unsigned char *evict_buf;

static inline void ll_swap(long long *a, long long *b)
{
    long long tmp = *a;
    *a = *b;
    *b = tmp;
}
static void swap_ll(long long *a, long long *b)
{
    long long temp = *a;
    *a = *b;
    *b = temp;
}


void init_evict() {
    evict_buf = aligned_alloc(64, EVICT_SIZE);

    for(size_t i = 0; i < EVICT_SIZE; i++) {
        evict_buf[i] = (unsigned char)i;
    }
}

void evict_cache() {

    volatile unsigned long long sink = 0;

    for(size_t i = 0; i < EVICT_SIZE; i += 64) {
        sink += evict_buf[i];
    }

    (void)sink;
}


static int partition(long long *v,
                     int low,
                     int high)
{
    long long pivot = v[high];

    int i = low - 1;

    for(int j = low; j < high; j++){

        if(v[j] <= pivot){
            i++;
            ll_swap(&v[i], &v[j]);
        }
    }

    ll_swap(&v[i + 1], &v[high]);

    return i + 1;
}

static void quicksort_ll(long long *v,
                         int low,
                         int high)
{
    if(low < high){

        int pi = partition(v, low, high);

        quicksort_ll(v, low, pi - 1);

        quicksort_ll(v, pi + 1, high);
    }
}
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

static void build_limits_sp2_serial(
    const long long *Input,
    long long n,
    int npivots,
    int nbins,
    long long *pivots,
    long long *limits)
{
    long long stride = n / npivots;

    // escolhe pivots
    for(int i = 0; i < npivots; i++){

        unsigned long long jitter =
            rand63() % stride;

        long long pos =
            i * stride + jitter;

        pivots[i] = Input[pos];
    }

    // ordena pivots
    quicksort_ll(pivots, 0, npivots - 1);

    // constroi limits
    limits[0] = LLONG_MIN;

    for(int i = 0; i < npivots; i++){
        limits[i + 1] = pivots[i];
    }

    limits[nbins] = LLONG_MAX;
}


void randomizador(long long n_elementos,int tb2, int numero_faixas_hist, long long *vetor_final)
{
    

    if(vetor_final == NULL){
        fprintf(stderr, "erro ao alocar memória para vetor_final\n");
        exit(1);
    }
    int count =0;
    for(unsigned long long i = 0; i < n_elementos; i++){
        vetor_final[i] = rand64();
        count++;
    }

    if(tb2 == 1){
/*        gen_test_data_balanced2(vetor_final,
                                n_elementos,
                                numero_faixas_hist);
    */
   printf("entrou no loop tb2 -> quantidade de numeros gerados->> %d \n", count);
   }

    return vetor_final;
}


//! _____________________________________________________________________ MAIN ________________________________________________________________ ///////////////////////////////////////


int main(int argc, char *argv[])
{
    if(argc < 5){
        fprintf(stderr, "Uso inválido\n");
        return 1;
    }

    long long numero_elementos = atoll(argv[1]);
   int numero_separadores_pivot = atoi(argv[2]);
    int numero_faixas_histograma = atoi(argv[3]);
    int numero_threads = atoi(argv[4]);
    int numero_rodadas_execucao = atoi(argv[5]);


    int tb2 = 0;


    /*
     Passo 1 — Geração do input
      Gera nelements valores long long aleatórios cobrindo
      uniformemente todo o intervalo [LLONG_MIN, LLONG_MAX]. Um segundo
      array idêntico (data2) é criado por memcpy — os dois arrays são
      fisicamente separados na memória para garantir que cada pool de
      threads leia da RAM, não do cache aquecido pelo outro.
*/
    if(argc > 6 && strcmp(argv[6], "-tb2") == 0){
        tb2 = 1;
    }
    long long *vetor1 = malloc(numero_elementos * sizeof(long long));
    long long *vetor2 = malloc(numero_elementos * sizeof(long long));
    randomizador(numero_elementos, tb2, numero_faixas_histograma, vetor1);
    memcpy(vetor2, vetor1, numero_elementos * sizeof(long long));
    
    /*
      Passo 2 — Construção dos limites (serial)
      Cronometrada independentemente. Produz limits[0..nbins] pelo modo
      escolhido. Este array é compartilhado por ambos os pools —
      garante que ambas as versões (1 thread e N threads) calculem o
      histograma sobre os mesmos intervalos.
    */
   //static void build_limits_sp2_serial(const long long *Input, long long n, int npivots,int nbins,long long *pivots,long long *limits ){

    long long *limits = (long long*) malloc(numero_faixas_histograma+1 * sizeof(long long));
    long long *pivots = (long long*) malloc(numero_separadores_pivot * sizeof(long long));

    
    build_limits_sp2_serial(vetor1, numero_elementos, numero_separadores_pivot, numero_faixas_histograma, pivots, limits);

    free(limits);
    free(vetor1);
    free(vetor2);
    return 0;
}