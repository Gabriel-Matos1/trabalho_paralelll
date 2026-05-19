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

#include <stdbool.h>
#include "chrono.c"

//!-------------------------------------------------------VARIÁVEIS_AAAAAAAAAAAAAAAAAAAAAAAAA----------------------------------------------------------------------------------------------------

#define DEBUG 0
// #define DEBUG 1
#define MAX_THREADS 64
#define LOOP_COUNT 1

#define MAX_TOTAL_ELEMENTS (500 * 1000 * 1000) // if each float takes 4 bytes
unsigned long long LLC_SIZE;
unsigned long long ESVAZIA_CACHE;

//
static unsigned char *CACHE_BUFFER;

pthread_t parallel_Thread[MAX_THREADS];
int parallel_thread_id[MAX_THREADS];
int paralle_nThreads;
long long parallel_nTotalElements;
long long *paralle_limites;
long long *parallel_data;
int paralel_n_faixas;
pthread_barrier_t parallel_barrier;
long long **local_hist;
long long *paralel_hist;
//

//!-------------------------------------------------------FUNÇÕES_AAAAAAAAAAAAAAAAAAAAAAAAA----------------------------------------------------------------------------------------------------

static unsigned long long descobrir_tamanho_L3()
{
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cache/index3/size", "r");

    if (!f)
    {
        perror("Erro ao abrir o arquivo de tamanho do cache");
        exit(1);
    }

    unsigned long long tamanho;
    char unit;

    if (fscanf(f, "%llu%c", &tamanho, &unit) != 2)
    {
        fprintf(stderr, "erro lendo tamanho do LLC\n");
        fclose(f);
        exit(1);
    }

    fclose(f);

    if (unit == 'K')
    {
        tamanho *= 1024ULL;
    }
    else if (unit == 'M')
    {
        tamanho *= 1024ULL * 1024ULL;
    }

    return tamanho;
}

static int busca_binaria(   long long valor,long long *limits,int n_faixa){
   
    int comeco = 0;
    int fim = n_faixa - 1;

    while(comeco <= fim){
        int meio = (comeco + fim) / 2;

        if(valor < limits[meio]){
            fim = meio - 1;
        }else if(valor >= limits[meio + 1]){
            comeco = meio + 1;
        }else{
            return meio;
        }
    }
    return n_faixa - 1;

}
//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

static inline void ll_swap(long long *a, long long *b)
{
    long long tmp = *a;
    *a = *b;
    *b = tmp;
}

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

static void inicia_vetor_cache()
{
    CACHE_BUFFER = aligned_alloc(64, ESVAZIA_CACHE);

    for (size_t i = 0; i < ESVAZIA_CACHE; i++)
    {
        CACHE_BUFFER[i] = (unsigned char)i;
    }
}

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

void cache_evadir()
{

    volatile unsigned long long valor_acumulador = 0;

    for (size_t i = 0; i < ESVAZIA_CACHE; i += 64)
    {
        valor_acumulador += CACHE_BUFFER[i];
    }

    (void)valor_acumulador;
}

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

static int partition(long long *v, int menor, int maior)
{
    long long pivot = v[maior];

    int i = menor - 1;

    for (int j = menor; j < maior; j++)
    {

        if (v[j] <= pivot)
        {
            i++;
            ll_swap(&v[i], &v[j]);
        }
    }

    ll_swap(&v[i + 1], &v[maior]);

    return i + 1;
}

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

static void quicksort_ll(long long *v, int menor, int maior)
{
    if (menor < maior)
    {

        int pi = partition(v, menor, maior);

        quicksort_ll(v, menor, pi - 1);

        quicksort_ll(v, pi + 1, maior);
    }
}

//** FUNÇÕES DADAS PELO PROFESSOR */

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

static int verify_histogram(const long long *data, long long nelements, const long long *limits, int n_faixas, const long long *hist_1thr, const long long *hist_nthr)
{
    /* Stage 1: 1-thread vs N-thread */
    int s1_ok = 1;
    for (int b = 0; b < n_faixas; b++)
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
     * Completely independent of achar_qual_faixa and the thread pool.         */
    long long *recount = (long long *)calloc(n_faixas, sizeof(long long));
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
        while (b < n_faixas - 1 && v >= limits[b + 1])
            b++;
        recount[b]++;
    }

    int s2_ok = 1;
    for (int b = 0; b < n_faixas; b++)
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
    for (int b = 0; b < n_faixas; b++)
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
    if (npivots < nbins || npivots > n)
    {
        fprintf(stderr, "erro: npivots invalido\n");
        exit(1);
    }

    long long stride = n / npivots;

    // escolhe pivots
    for (int i = 0; i < npivots; i++)
    {
        unsigned long long jitter =
            rand63() % stride;

        long long pos =
            i * stride + jitter;

        pivots[i] = Input[pos];
    }

    // ordena pivots
    quicksort_ll(pivots, 0, npivots - 1);

    // extremos
    limits[0] = LLONG_MIN;

    // seleciona limites
    for (int j = 1; j < nbins; j++)
    {
        int pidx =
            j * (npivots - 1) / nbins;

        limits[j] = pivots[pidx];
    }

    limits[nbins] = LLONG_MAX;

    // empurrão suave
    for (int k = 1; k <= nbins - 1; k++)
    {
        if (limits[k] <= limits[k - 1])
        {
            if (limits[k - 1] < LLONG_MAX - 1)
                limits[k] = limits[k - 1] + 1;
            else
                limits[k] = limits[k - 1];
        }
    }
}
//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

static void gen_test_data_balanced2(long long *data, long long nelements, int n_faixas)
{
    if (nelements <= 0 || n_faixas <= 0)
        return;

    for (long long i = 0; i < nelements; i++)
        data[i] = i;

    for (long long i = nelements - 1; i > 0; i--)
    {
        long long j = (long long)(rand63() % (unsigned long long)(i + 1));
        ll_swap(&data[i], &data[j]);
    }

    for (long long i = 0; i < nelements; i++)
        data[i] %= n_faixas;
}

//**----------------------------------------------------- Funções criadas ------------------------------------------------------------------------------------////////////

void histogram_serial(long long *data, long long n, long long *limits, int n_faixas, long long *hist){
    for (int b = 0; b < n_faixas; b++){
        hist[b] = 0;
    }
    for (long long i = 0; i < n; i++){

        int bin = busca_binaria(data[i], limits, n_faixas);

        hist[bin]++;
    }
}

void *parallel_hist_worker(void *ptr)
{
    int tid = *(int *)ptr;

    while (true)
    {

        pthread_barrier_wait(&parallel_barrier);

        long long chunk =
            (parallel_nTotalElements + paralle_nThreads - 1) / paralle_nThreads;

        long long first = tid * chunk;

        long long last = first + chunk;

        if (last > parallel_nTotalElements)
            last = parallel_nTotalElements;
        for (int b = 0; b < paralel_n_faixas; b++)
            local_hist[tid][b] = 0;

        for (long long i = first; i < last; i++)
        {
            int faixa =busca_binaria(parallel_data[i], paralle_limites, paralel_n_faixas);

            local_hist[tid][faixa]++;
        }

        pthread_barrier_wait(&parallel_barrier);
    }

    return NULL;
}

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------
void parallel_hist(long long *data, long long n, long long *limits, int n_faixas, long long *hist, int nThreads)
{
    static int initialized = 0;

    parallel_data = data;
    parallel_nTotalElements = n;
    paralle_limites = limits;
    paralel_n_faixas = n_faixas;
    paralle_nThreads = nThreads;

    if(nThreads == 1){
    histogram_serial(data, n, limits, n_faixas, hist);
        return;
    }
    if (!initialized)
    {

        pthread_barrier_init(&parallel_barrier,NULL,nThreads + 1);

        local_hist = malloc(nThreads * sizeof(long long *));

        for (int t = 0; t < nThreads; t++){
            local_hist[t] =calloc(n_faixas, sizeof(long long));
        }

        for (int t = 0; t < nThreads; t++){
            parallel_thread_id[t] = t;

            pthread_create(&parallel_Thread[t], NULL, parallel_hist_worker, &parallel_thread_id[t]);
        }

        initialized = 1;
    }

    pthread_barrier_wait(&parallel_barrier);

    pthread_barrier_wait(&parallel_barrier);
    for (int b = 0; b < n_faixas; b++)
        hist[b] = 0;

    for (int t = 0; t < nThreads; t++)
    {
        for (int b = 0; b < n_faixas; b++)
        {
            hist[b] += local_hist[t][b];
        }
    }
}
//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

//!-----------------------------------------------------------------------------------------------------------------------------------------------------------

void randomizador(long long n_elementos, int tb2, int numero_faixas_hist, long long *vetor_final){

    if (vetor_final == NULL){
        fprintf(stderr, "erro ao alocar memória para vetor_final\n");
        exit(1);
    }
    int count = 0;
    for (long long i = 0; i < n_elementos; i++){
        vetor_final[i] = rand64();
        count++;
    }

    if (tb2 == 1){
        gen_test_data_balanced2(vetor_final, n_elementos, numero_faixas_hist);

    }

    return;
}

//! _____________________________________________________________________ MAIN ________________________________________________________________ ///////////////////////////////////////

int main(int argc, char *argv[])
{
    if (argc < 6)
    {
        fprintf(stderr, "uso: %s nelements npivots n_faixas nthreads nr [ -tb2 ]\n",
                argv[0]);
        return 1;
    }

    long long numero_elementos = atoll(argv[1]);
    int numero_separadores_pivot = atoi(argv[2]);
    int numero_faixas_histograma = atoi(argv[3]);
    int numero_threads = atoi(argv[4]);
    int numero_rodadas_execucao = atoi(argv[5]);
    double soma_bl = 0.0;
    double soma_serial = 0.0;
    double soma_parallel = 0.0;
    double soma_speedup = 0.0;
    bool tot_ok = true;
    
    int tb2 = 0;
    char tb2_s[7] = "";

    if (argc > 6 && strcmp(argv[6], "-tb2") == 0)
    {
        tb2 = 1;
        strcpy(tb2_s, "(-tb2)");
    }

    LLC_SIZE = descobrir_tamanho_L3();
    ESVAZIA_CACHE = 3ULL * LLC_SIZE;
    inicia_vetor_cache();

    printf("=== Parallel Histogram — Scalability Test (Persistent Thread Pool) ===\n");
    printf("  Elements : %lld  |  Pivots : %d  |  Bins : %d  |  Threads : %d  |  Rounds : %d  |  Input : uniform random in [0,n_faixas) %s\n", numero_elementos, numero_separadores_pivot, numero_faixas_histograma, numero_threads, numero_rodadas_execucao, tb2_s);
    printf("  LLC size : %llu MiB  |  Eviction buffer : %llu MiB\n\n", LLC_SIZE / (1024ULL * 1024ULL), ESVAZIA_CACHE / (1024ULL * 1024ULL));

    for (int r = 0; r < numero_rodadas_execucao; r++)
    {

        long long *vetor1 = malloc(numero_elementos * sizeof(long long));

        long long *vetor2 = malloc(numero_elementos * sizeof(long long));

        if (!vetor1 || !vetor2)
        {
            fprintf(stderr, "Erro ao alocar os vetores para os histogramas");
            exit(1);
        }
        randomizador(numero_elementos, tb2, numero_faixas_histograma, vetor1);

        memcpy(vetor2, vetor1, numero_elementos * sizeof(long long));

//!----------------------limites---------------------------

        long long *limits = malloc((numero_faixas_histograma + 1) * sizeof(long long));

        long long *pivots = malloc(numero_separadores_pivot * sizeof(long long));

        chronometer_t chrono_blser;

        chrono_reset(&chrono_blser);
        chrono_start(&chrono_blser);
        build_limits_sp2_serial(vetor1, numero_elementos, numero_separadores_pivot, numero_faixas_histograma, pivots, limits);
        chrono_stop(&chrono_blser);
//!----------------------calcula histograma---------------------------

        long long *hist_serial = calloc(numero_faixas_histograma, sizeof(long long));

        long long *hist_parallel = calloc(numero_faixas_histograma, sizeof(long long));

        cache_evadir();

//!----------------------fazer com serial---------------------------

        chronometer_t chrono_serial;

        chrono_reset(&chrono_serial);
        chrono_start(&chrono_serial);

        histogram_serial(vetor1, numero_elementos, limits, numero_faixas_histograma, hist_serial);

        chrono_stop(&chrono_serial);

        cache_evadir();
//!----------------------fazer com paralelo---------------------------
        chronometer_t chrono_parallel;

        chrono_reset(&chrono_parallel);
        chrono_start(&chrono_parallel);

        parallel_hist(vetor2, numero_elementos, limits, numero_faixas_histograma, hist_parallel, numero_threads);

        chrono_stop(&chrono_parallel);

//!----------------------verificação com a função do veirfy---------------------------
        int ok = verify_histogram(vetor1, numero_elementos, limits, numero_faixas_histograma, hist_serial, hist_parallel);

//!----------------------calcular tempo do round---------------------------
        if (r == 0){
            printf("\nROUND %d: first 8 partitions\n", r);
            printf("  Bin  ;          Lo (inclusive)  ;          Hi (exclusive)  ;         Count\n");
            for (int p = 0; p < 8 && p < numero_faixas_histograma; p++){
                printf("%6d ; %24lld ; %24lld ; %15lld\n",p,limits[p],limits[p + 1],hist_serial[p]);

            }
            printf("\nROUND %d: first 8 partitions\n", r);
            printf("Round ;  T(bl_ser) s ;    T(1 thr) s ;   T(N thr) s ;    Speedup ; OK?\n");
            printf("----- ;------------- ;-------------- ;------------- ;----------- ; ----\n");
        }
        double tempo_serial = (double)chrono_gettotal(&chrono_serial) / 1e9;

        double tempo_parallel = (double)chrono_gettotal(&chrono_parallel) / 1e9;

        double tempo_bl_ser = (double)chrono_gettotal(&chrono_blser) / 1e9;
        double speedup = tempo_serial / tempo_parallel;

        soma_bl += tempo_bl_ser;
        soma_serial += tempo_serial;
        soma_parallel += tempo_parallel;
        soma_speedup += speedup;

        printf("  %-5d ; %12.6lf ; %12.6lf ; %12.6lf ; %10.3lf ; %s\n", r + 1, tempo_bl_ser, tempo_serial, tempo_parallel, speedup, ok ? "OK" : "FAIL");

        if(!ok){tot_ok = false;}
        free(hist_serial);
        free(hist_parallel);

        free(pivots);
        free(limits);

        free(vetor1);
        free(vetor2);
    }

    printf("  ----- ;------------ ;----------- ;------------ ;---------- ; ----\n");
    char *resultado_over = "PASS";

    if(!tot_ok){
        resultado_over = "NOT PASS";
    }
    printf("  AVG   ; %12.6lf ; %12.6lf ; %12.6lf ; %10.3lf ; %s\n",soma_bl / numero_rodadas_execucao,soma_serial / numero_rodadas_execucao,soma_parallel / numero_rodadas_execucao,soma_speedup / numero_rodadas_execucao, tot_ok? "OK": "FAILA");

    printf("  ----- ;------------ ;----------- ;------------ ;---------- ; ----\n");

    double avg_bl =soma_bl / numero_rodadas_execucao;
    double avg_serial =soma_serial / numero_rodadas_execucao;
    double avg_parallel =soma_parallel / numero_rodadas_execucao;
    double avg_speedup =soma_speedup / numero_rodadas_execucao;
    double meps_serial =(double)numero_elementos /(avg_serial * 1e6);
    double meps_parallel =(double)numero_elementos /(avg_parallel * 1e6);
    double eficiencia =(avg_speedup / numero_threads) * 100.0;

    printf("\n=== Summary ===\n");

    printf("  Avg build_limits serial   : %.6lf s\n",avg_bl);

    printf("  Avg time  (1 thread )     : %.6lf s ; %.2lf MEPS\n",avg_serial,meps_serial);

    printf("  Avg time  (%d threads)    : %.6lf s ; %.2lf MEPS\n",numero_threads,avg_parallel,meps_parallel);

    printf("  Avg histogram speedup     : %.3lfx\n\n",avg_speedup);

    printf("  Parallel efficiency:\n");
    printf("  with nthreads (%d)        : %.1lf%%\n\n",numero_threads,eficiencia);

    printf("  Overall correctness       : %s\n",resultado_over);

    free(CACHE_BUFFER);

    return 0;
}

/*
 1513  ./"teste" 100 10 32 8 10 -tb2
 1514  ./"teste" 100 10 32 8 10
*/
