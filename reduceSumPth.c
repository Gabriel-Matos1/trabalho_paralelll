#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sys/time.h>
#include <stdlib.h>


#include "chrono.c"

// REDUÇÃO paralela com Pthreads 
// por W.Zola/UFPR

// Versão 1.0
//  - versão inicial
// versa0 1.1 (out/2024)
//  - revisões e alterações gerais de bugs

// (versao com thread pool simples)
// --------------------------------

// será feita a redução com operação de soma de floats (operação +, somando dois floats)
// usando nThreads (definida via linha de comando)

// Entrada para a operação ReducePlus será: 
//   um vetor de nTotalElements elementos 
//   (nTotalElements obtido da linha de comando)
// Para esse teste o vetor NÂO será lido, 
//   - o vetor será preenchido sequencialmente pela função main

// Cada thread terá irá fazer a redução de nTotalElements / nThreads elementos
// A última thread poderá reduzir menos que nTotalElements / nThreads elementos
//   quando n não for múltiplo de nThreads
// Cada thread irá produzir seu resultado no vetor partialSum
// Todas as threads sincronizam com uma barreira
// Após sincronização com barreira:
//   - a thread 0 (main) irá calcular e imprimir a soma global sequencialmente 
//   - as threads diferentes de 0 irão simplesmente terminar

// o programa deve calcular e imprimir o tempo de calculo da reducao
// em duas versoes:
// versão A) NAO incluindo o tempo de criação das threads
// versão B) INCLUINDO  o tempo de criação das threads

// rodar o programa 30 vezes obtendo o tempo MÍNIMO e o MÉDIO
//  nas duas versoes

// calcular a aceleração para 2, 3, 4, 5, 6, 7, e 8 threads
// colocando em uma planilha (tabela)

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
              printf( "usage: %s <nTotalElemegit push -u origin mainnts> <nThreads>\n" ,
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
