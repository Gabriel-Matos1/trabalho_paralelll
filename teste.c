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

//!-------------------------------------------------------VARIÁVEIS_AAAAAAAAAAAAAAAAAAAAAAAAA----------------------------------------------------------------------------------------------------

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

pthread_t parallel_Thread[MAX_THREADS];
int parallel_thread_id[MAX_THREADS];
int paralle_nThreads;
long long parallel_nTotalElements;
long long *paralle_limites;
long long *parallel_data;
int paralel_nbins;
pthread_barrier_t parallel_barrier;
long long **local_hist;
long long *paralel_hist;
//

//!-------------------------------------------------------FUNÇÕES_AAAAAAAAAAAAAAAAAAAAAAAAA----------------------------------------------------------------------------------------------------

static int find_bin(
    long long value,
    long long *limits,
    int nbins)
{
    for (int b = 0; b < nbins - 1; b++)
    {

        if (value < limits[b + 1])
            return b;
    }

    return nbins - 1;
}

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

static inline void ll_swap(long long *a, long long *b)
{
    long long tmp = *a;
    *a = *b;
    *b = tmp;
}

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

void init_evict()
{
    evict_buf = aligned_alloc(64, EVICT_SIZE);

    for (size_t i = 0; i < EVICT_SIZE; i++)
    {
        evict_buf[i] = (unsigned char)i;
    }
}

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

void evict_cache()
{

    volatile unsigned long long sink = 0;

    for (size_t i = 0; i < EVICT_SIZE; i += 64)
    {
        sink += evict_buf[i];
    }

    (void)sink;
}

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

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

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

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

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

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

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

static inline unsigned long long rand63(void)
{
    return ((unsigned long long)(unsigned)rand()) | ((unsigned long long)(unsigned)rand() << 21) | ((unsigned long long)(unsigned)rand() << 42);
}

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

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

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

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

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

static void gen_test_data_balanced2(long long *data,
                                    long long nelements,
                                    int nbins)
{
    if (nelements <= 0 || nbins <= 0)
        return;

    for (long long i = 0; i < nelements; i++)
        data[i] = i;

    for (long long i = nelements - 1; i > 0; i--)
    {
        long long j = (long long)(rand63() % (unsigned long long)(i + 1));
        ll_swap(&data[i], &data[j]);
    }

    for (long long i = 0; i < nelements; i++)
        data[i] %= nbins;
}

//!----------------------------------------------------- funções criadas ------------------------------------------------------------------------------------////////////
void *parallel_hist_worker(void *ptr)
{
    int tid = *(int *)ptr;

    while(true)
    {
        // espera nova tarefa
        pthread_barrier_wait(&parallel_barrier);

        long long chunk =
            (parallel_nTotalElements + paralle_nThreads - 1)
            / paralle_nThreads;

        long long first = tid * chunk;

        long long last = first + chunk;

        if(last > parallel_nTotalElements)
            last = parallel_nTotalElements;

        // limpa histograma local
        for(int b = 0; b < paralel_nbins; b++)
            local_hist[tid][b] = 0;

        // processa
        for(long long i = first; i < last; i++)
        {
            int bin =
                find_bin(parallel_data[i],
                         paralle_limites,
                         paralel_nbins);

            local_hist[tid][bin]++;
        }

        // espera todas terminarem
        pthread_barrier_wait(&parallel_barrier);

        // thread 0 retorna para caller
        if(tid == 0)
            return NULL;
    }
}

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------
void parallel_hist(
    long long *data,
    long long n,
    long long *limits,
    int nbins,
    long long *hist,
    int nThreads)
{
    static int initialized = 0;

    parallel_data = data;
    parallel_nTotalElements = n;
    paralle_limites = limits;
    paralel_nbins = nbins;
    paralle_nThreads = nThreads;
    paralel_hist = hist;

    if(!initialized)
    {
        pthread_barrier_init(
            &parallel_barrier,
            NULL,
            nThreads
        );

        local_hist =
            malloc(nThreads * sizeof(long long*));

        for(int t = 0; t < nThreads; t++)
        {
            local_hist[t] =
                calloc(nbins, sizeof(long long));
        }

        parallel_thread_id[0] = 0;

        for(int t = 1; t < nThreads; t++)
        {
            parallel_thread_id[t] = t;

            pthread_create(
                &parallel_Thread[t],
                NULL,
                parallel_hist_worker,
                &parallel_thread_id[t]
            );
        }

        initialized = 1;
    }

    // MAIN É THREAD 0
    parallel_hist_worker(&parallel_thread_id[0]);

    // merge final
    for(int b = 0; b < nbins; b++)
        hist[b] = 0;

    for(int t = 0; t < nThreads; t++)
    {
        for(int b = 0; b < nbins; b++)
        {
            hist[b] += local_hist[t][b];
        }
    }
}
//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

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

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

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
/**
 * 
Para cada round r = 1..nr o programa executa os seguintes passos:

  Passo 1 — Geração do input
      Gera nelements valores long long aleatórios cobrindo
      uniformemente todo o intervalo [LLONG_MIN, LLONG_MAX]. Um segundo
      array idêntico (data2) é criado por memcpy — os dois arrays são
      fisicamente separados na memória para garantir que cada pool de
      threads leia da RAM, não do cache aquecido pelo outro.

  Passo 2 — Construção dos limites (serial)
      Cronometrada independentemente. Produz limits[0..nbins] pelo modo
      escolhido. Este array é compartilhado por ambos os pools —
      garante que ambas as versões (1 thread e N threads) calculem o
      histograma sobre os mesmos intervalos.

  Passo 3 — Evicção de cache
      Entre cada operação cronometrada, um buffer de 3 × LLC bytes é
      lido sequencialmente para garantir que data e data2 sejam lidos
      da RAM (não do cache) durante as medições.

  Passo 4 — Histograma com 1 thread 
      executa o histograma sobre data com 1 thread (o próprio
      main, sem pthread extra). O tempo T(1 thr) é medido.

  Passo 5 — Histograma com N threads (pool de threads)
      um  pool de threads executa o histograma sobre data2 com nthreads threads
      (incluindo main como thread trabalhadora - i.e. worker). O tempo T(N thr) é medido.

  Passo 6 — Verificação de corretude
      A nova funcao de verificacao de resultados foi DADA pelo prof.
      (estah no ANEXO 2 ao final dessa especificacao)
      
         Voce DEVE usar essa funcao para verificar os resultados
         e reportar erros, conforme especificado
 * 
 */
int main(int argc, char *argv[])
{
    if(argc < 6) {
        fprintf(stderr, "uso: %s nelements npivots nbins nthreads nr [ -tb2 ]\n",
                argv[0]);
        return 1;
    }

    long long numero_elementos = atoll(argv[1]);
    int numero_separadores_pivot = atoi(argv[2]);
    int numero_faixas_histograma = atoi(argv[3]);
    int numero_threads = atoi(argv[4]);
    int numero_rodadas_execucao = atoi(argv[5]);

    int tb2 = 0;

    if(argc > 6 && strcmp(argv[6], "-tb2") == 0)
        tb2 = 1;

    init_evict();

    for(int r = 0; r < numero_rodadas_execucao; r++) {

        printf("\nROUND %d\n", r);

        // =========================================================
        // PASSO 1 - geração dos dados
        // =========================================================

        long long *vetor1 =
            malloc(numero_elementos * sizeof(long long));

        long long *vetor2 =
            malloc(numero_elementos * sizeof(long long));

        randomizador(numero_elementos,
                     tb2,
                     numero_faixas_histograma,
                     vetor1);

        memcpy(vetor2,
               vetor1,
               numero_elementos * sizeof(long long));

        // =========================================================
        // PASSO 2 - construção dos limites
        // =========================================================

        long long *limits =
            malloc((numero_faixas_histograma + 1)
                   * sizeof(long long));

        long long *pivots =
            malloc(numero_separadores_pivot
                   * sizeof(long long));

        build_limits_sp2_serial(
            vetor1,
            numero_elementos,
            numero_separadores_pivot,
            numero_faixas_histograma,
            pivots,
            limits);

        // =========================================================
        // histogramas
        // =========================================================

        long long *hist_serial =
            calloc(numero_faixas_histograma,
                   sizeof(long long));

        long long *hist_parallel =
            calloc(numero_faixas_histograma,
                   sizeof(long long));

        // =========================================================
        // PASSO 3 - flush cache
        // =========================================================

        evict_cache();

        // =========================================================
        // PASSO 4 - serial
        // =========================================================

        chronometer_t chrono_serial;

        chrono_reset(&chrono_serial);
        chrono_start(&chrono_serial);

        histogram_serial(
            vetor1,
            numero_elementos,
            limits,
            numero_faixas_histograma,
            hist_serial);

        chrono_stop(&chrono_serial);

        // =========================================================
        // PASSO 3 novamente - flush cache
        // =========================================================

        evict_cache();

        // =========================================================
        // PASSO 5 - paralelo
        // =========================================================

        chronometer_t chrono_parallel;

        chrono_reset(&chrono_parallel);
        chrono_start(&chrono_parallel);

        parallel_hist(
            vetor2,
            numero_elementos,
            limits,
            numero_faixas_histograma,
            hist_parallel,
            numero_threads);

        chrono_stop(&chrono_parallel);

        // =========================================================
        // PASSO 6 - verificação
        // =========================================================

        int ok = verify_histogram(
            vetor1,
            numero_elementos,
            limits,
            numero_faixas_histograma,
            hist_serial,
            hist_parallel);

        if(ok)
            printf("verify_histogram: OK\n");
        else
            printf("verify_histogram: FAIL\n");

        // =========================================================
        // tempos
        // =========================================================

        double tempo_serial =
            (double)chrono_gettotal(&chrono_serial)
            / 1e9;

        double tempo_parallel =
            (double)chrono_gettotal(&chrono_parallel)
            / 1e9;

        printf("serial   = %lf s\n", tempo_serial);
        printf("parallel = %lf s\n", tempo_parallel);
        printf("speedup  = %lf\n",
               tempo_serial / tempo_parallel);

        // =========================================================
        // frees
        // =========================================================

        free(hist_serial);
        free(hist_parallel);

        free(pivots);
        free(limits);

        free(vetor1);
        free(vetor2);
    }

    free(evict_buf);

    return 0;
}


/*
 1513  ./"teste" 100 10 32 8 10 -tb2
 1514  ./"teste" 100 10 32 8 10 
*/