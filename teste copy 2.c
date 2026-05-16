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
#include <stdbool.h>
#define DEBUG 0
// #define DEBUG 1
#define MAX_THREADS 64
#define LOOP_COUNT 1



#define MAX_TOTAL_ELEMENTS (500 * 1000 * 1000) // if each float takes 4 bytes
                                               // will have a maximum 500 million FLOAT elements
#define LLC_SIZE (12ULL * 1024 * 1024)
#define EVICT_SIZE (3ULL * LLC_SIZE)


//
static unsigned char *evict_buf;

pthread_t parallel_Thread[ MAX_THREADS ];
int parallel_thread_id[ MAX_THREADS ];
int paralle_nThreads;    
long long parallel_nTotalElements; 
long long *paralle_limites;
long long *parallel_data;
int paralel_nbins;
pthread_barrier_t parallel_barrier;
long long **local_hist;
//



static int find_bin(
    long long value,
    long long *limits,
    int nbins)
{
    for(int b = 0; b < nbins - 1; b++){

        if(value < limits[b + 1])
            return b;
    }

    return nbins - 1;
}

static inline void ll_swap(long long *a, long long *b)
{
    long long tmp = *a;
    *a = *b;
    *b = tmp;
}

void init_evict()
{
    evict_buf = aligned_alloc(64, EVICT_SIZE);

    for (size_t i = 0; i < EVICT_SIZE; i++)
    {
        evict_buf[i] = (unsigned char)i;
    }
}

void evict_cache()
{

    volatile unsigned long long sink = 0;

    for (size_t i = 0; i < EVICT_SIZE; i += 64)
    {
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

    for (int j = low; j < high; j++)
    {

        if (v[j] <= pivot)
        {
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
    if (low < high)
    {

        int pi = partition(v, low, high);

        quicksort_ll(v, low, pi - 1);

        quicksort_ll(v, pi + 1, high);
    }
}



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
    long long nelements,
    const long long *limits,
    int nbins,
    const long long *hist_1thr,
    const long long *hist_nthr)
{
    /* Stage 1: 1-thread vs N-thread */
    int s1_ok = 1;
    for (int b = 0; b < nbins; b++)
    {
        if (hist_1thr[b] != hist_nthr[b])
        {
            fprintf(stderr,
                    "  VERIFY FAIL stage1: bin %d  1thr=%lld  Nthr=%lld\n",
                    b, hist_1thr[b], hist_nthr[b]);
            s1_ok = 0;
        }
    }
    if (!s1_ok)
        return 0;

    /* Stage 2: serial recount vs hist_nthr
     * Uses a simple linear scan: walk limits[] left to right.
     * Completely independent of find_bin and the thread pool.         */
    long long *recount = (long long *)calloc(nbins, sizeof(long long));
    if (!recount)
    {
        perror("calloc recount");
        return 0;
    }

    for (long long i = 0; i < nelements; i++)
    {
        long long v = data[i];
        int b = 0;
        /* Linear scan: find rightmost bin whose left edge <= v */
        while (b < nbins - 1 && v >= limits[b + 1])
            b++;
        recount[b]++;
    }

    int s2_ok = 1;
    for (int b = 0; b < nbins; b++)
    {
        if (recount[b] != hist_nthr[b])
        {
            fprintf(stderr,
                    "  VERIFY FAIL stage2: bin %d  recount=%lld  Nthr=%lld\n",
                    b, recount[b], hist_nthr[b]);
            s2_ok = 0;
        }
    }
    free(recount);
    if (!s2_ok)
        return 0;

    /* Stage 3: invariant — sum of bins == nelements */
    long long total = 0;
    for (int b = 0; b < nbins; b++)
        total += hist_nthr[b];
    if (total != nelements)
    {
        fprintf(stderr,
                "  VERIFY FAIL stage3: sum=%lld expected=%lld\n",
                total, nelements);
        return 0;
    }

    return 1; /* all stages passed */
}

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
    union
    {
        unsigned long long u;
        long long s;
    } x;

    x.u = ((unsigned long long)(unsigned)rand() << 49) | ((unsigned long long)(unsigned)rand() << 34) | ((unsigned long long)(unsigned)rand() << 19) | ((unsigned long long)(unsigned)rand() << 4) | ((unsigned long long)(unsigned)rand() & 0xF);

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

static void build_limits_sp2_serial(
    const long long *Input,
    long long n,
    int npivots,
    int nbins,
    long long *pivots,
    long long *limits)
{
    if (npivots > 0 && npivots < n)
    {
        long long stride = n / npivots;

        // escolhe pivots
        for (int i = 0; i < npivots; i++)
        {

            unsigned long long jitter = rand63() % stride;

            long long pos = i * stride + jitter;

            pivots[i] = Input[pos];
        }

        // ordena pivots
        quicksort_ll(pivots, 0, npivots - 1);

        // constroi limits
        limits[0] = LLONG_MIN;

        for (int i = 0; i < npivots; i++)
        {
            limits[i + 1] = pivots[i];
        }

        limits[nbins] = LLONG_MAX;
    }
    else
    {
        fprintf(stderr, "inflação: Pivots = 0");
        exit(1);
    }
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

//!----------------------------------------------------- funções criadas ------------------------------------------------------------------------------------////////////

void *parallel_hist_worker(void *ptr)
{
    int tid = *(int*)ptr;

    long long chunk =
        (parallel_nTotalElements + paralle_nThreads - 1)
        / paralle_nThreads;

    long long first = tid * chunk;

    long long last =
        first + chunk;

    if(last > parallel_nTotalElements)
        last = parallel_nTotalElements;

    for(long long i = first; i < last; i++) {

        int bin = find_bin(parallel_data[i], paralle_limites, paralel_nbins);

        local_hist[tid][bin]++;
    }

    return NULL;
}

void parallel_hist( 
    long long *data,
    long long n,
    long long *limits,
    int nbins,
    long long *hist, int nThreads)
{

    static int initialized = 0;
    parallel_nTotalElements = n;
    paralle_nThreads = nThreads;
    paralle_limites = limits;
    parallel_data = data;
paralel_nbins = nbins;
    if( ! initialized ) { 
       pthread_barrier_init( &parallel_barrier, NULL, nThreads );
       // thread 0 will be the caller thread
    
       // cria todas as outra threds trabalhadoras
       parallel_thread_id[0] = 0;
       for( int i=1; i < nThreads; i++ ) {
         parallel_thread_id[i] = i;
         pthread_create( &parallel_Thread[i], NULL, 
                      parallel_hist_worker, &parallel_thread_id[i]);
       }

       initialized = 1;
    }

    //reducePartialSum( &parallel_thread_id[0] ); 
        
    return;
}


void histogram_serial(
    long long *data,
    long long n,
    long long *limits,
    int nbins,
    long long *hist)
{
    for (int b = 0; b < nbins; b++)
        hist[b] = 0;

    for (long long i = 0; i < n; i++)
    {

        int bin = find_bin(data[i], limits, nbins);

        hist[bin]++;
    }
}
void randomizador(long long n_elementos, int tb2, int numero_faixas_hist, long long *vetor_final)
{

    if (vetor_final == NULL)
    {
        fprintf(stderr, "erro ao alocar memória para vetor_final\n");
        exit(1);
    }
    int count = 0;
    for (long long i = 0; i < n_elementos; i++)
    {
        vetor_final[i] = rand64();
        count++;
    }

    if (tb2 == 1)
    {
               gen_test_data_balanced2(vetor_final,
                                        n_elementos,
                                        numero_faixas_hist);
            
        printf("entrou no loop tb2 -> quantidade de numeros gerados->> %d \n", count);
    }

    return;
}

//! _____________________________________________________________________ MAIN ________________________________________________________________ ///////////////////////////////////////

int main(int argc, char *argv[])
{
    if (argc < 6)
    {
        fprintf(stderr, "Uso inválido\n");
        return 1;
    }

    long long numero_elementos = atoll(argv[1]);
    int numero_separadores_pivot = atoi(argv[2]);
    int numero_faixas_histograma = atoi(argv[3]);
   //int numero_threads = atoi(argv[4]);
    int numero_rodadas_execucao = atoi(argv[5]);

    int tb2 = 0;
        if (argc > 6 && strcmp(argv[6], "-tb2") == 0)
        {
            tb2 = 1;
        }

    for(int t=0; t<numero_rodadas_execucao; t++){

        long long *vetor1 = malloc(numero_elementos * sizeof(long long));
        long long *vetor2 = malloc(numero_elementos * sizeof(long long));

        if (!vetor1 || !vetor2)
        {
            perror("malloc");
            exit(1);
        }

        randomizador(numero_elementos, tb2, numero_faixas_histograma, vetor1);
        memcpy(vetor2, vetor1, numero_elementos * sizeof(long long));

        /*
        Passo 2 — Construção dos limites (serial)
        Cronometrada independentemente. Produz limits[0..nbins] pelo modo
        escolhido. Este array é compartilhado por ambos os pools —
        garante que ambas as versões (1 thread e N threads) calculem o
        histograma sobre os mesmos intervalos.
        */

        long long *limits = (long long *)malloc((numero_faixas_histograma + 1) * sizeof(long long));
        long long *pivots = (long long *)malloc(numero_separadores_pivot * sizeof(long long));
        if (!limits || !pivots)
        {
            perror("malloc");
            exit(1);
        }

        build_limits_sp2_serial(vetor1, numero_elementos, numero_separadores_pivot, numero_faixas_histograma, pivots, limits);

        /*
        Passo 3 — Evicção de cache
        Entre cada operação cronometrada, um buffer de 3 × LLC bytes é
        lido sequencialmente para garantir que data e data2 sejam lidos
        da RAM (não do cache) durante as medições.*/

        init_evict();

        /* benchmark serial */
        evict_cache();
        chronometer_t cron_start;

        chrono_start(&cron_start);

        /*

        Passo 4 — Histograma com 1 thread
        executa o histograma sobre data com 1 thread (o próprio
        main, sem pthread extra). O tempo T(1 thr) é medido.
        */
        long long *hist = calloc(numero_faixas_histograma, sizeof(long long));

        if (!hist)
        {
            perror("malloc");
            exit(1);
        }
        histogram_serial(vetor1, numero_elementos, limits, numero_faixas_histograma, hist);
        chrono_stop(&cron_start);

        /*
        Passo 5 — Histograma com N threads (pool de threads)
        um  pool de threads executa o histograma sobre data2 com nthreads threads
        (incluindo main como thread trabalhadora - i.e. worker). O tempo T(N thr) é medido.
        */


        /*Passo 6 — Verificação de corretude
        A nova funcao de verificacao de resultados foi DADA pelo prof.
            (estah no ANEXO 2 ao final dessa especificacao)

                Voce DEVE usar essa funcao para verificar os resultados
                e reportar erros, conforme especificado

                É uma funcao MAIS completa do que a verificacao simples anterior.
        */

        int ok = verify_histogram(vetor1, numero_elementos, limits, numero_faixas_histograma, hist, hist);


        if(ok){
            printf("verify_histogram: OK\n");
        }else{ 
            printf("verify_histogram: FAIL\n");
        }

        free(pivots);

        free(evict_buf);
        free(limits);
        free(vetor1);
        free(vetor2);

    }

/*
    chrono_reportTime( &parallelReductionTime, "parallelReductionTime" );
    
    // calcular e imprimir a VAZAO (numero de operacoes/s)
    double total_time_in_seconds = (double) chrono_gettotal( &parallelReductionTime ) /
                                      ((double)1000*1000*1000);
    printf( "total_time_in_seconds: %lf s\n", total_time_in_seconds );
                                  
    double OPS = ((double)numero_elementos*numero_rodadas_execucao)/total_time_in_seconds;
    printf( "Throughput: %lf OP/s\n", OPS );
  */  
    return 0;
}