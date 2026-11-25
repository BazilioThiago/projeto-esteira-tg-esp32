/* M3 - Guilherme Santana e Thiago Bazilio.
 * Porta a operacao SORT da M2 para pthreads no Linux.
 * Base: data/codigos_viel/Aperiodic_Server_ex.c (Felipe Viel).
 * gcc -std=c11 -O2 -Wall -Wextra -Werror servidor_sort.c -o servidor_sort -pthread
 * sudo ./servidor_sort normal 10 50 20
 * Argumentos: cenario, duracao(s), Ts(ms), Cs(ms).
 * O exemplo executa jobs atomicos: o ultimo job pode ultrapassar Cs.
 * Essa ultrapassagem e medida, nunca escondida na analise.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NS_MS INT64_C(1000000)
#define QUEUE_CAP 32
#define MAX_RECORDS 20000
#define SORT_DEADLINE (10 * NS_MS)

typedef struct { unsigned id; int64_t event; } job_t;
typedef struct {
    char type; unsigned id, task;
    int64_t release, start, end, cpu, deadline;
    unsigned depth;
} record_t;
typedef struct { unsigned id; int period_ms, priority; } periodic_t;
static periodic_t periodic[4] = {{0,40,50},{1,80,49},{2,120,48},{3,160,47}};
static job_t queue[QUEUE_CAP];
static unsigned head, tail, depth, produced, accepted, rejected, completed, max_depth;
static unsigned production, rec_count, rec_lost;
static record_t records[MAX_RECORDS];
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t belt_mutex = PTHREAD_MUTEX_INITIALIZER;
static int64_t epoch, stop_at, ts, cs;
static const char *scenario;
static int cpu_core;

static void check(int rc, const char *what) {
    if (rc) { fprintf(stderr, "%s: %s\n", what, strerror(rc)); exit(EXIT_FAILURE); }
}
static int64_t now_ns(clockid_t clock_id) {
    struct timespec t;
    if (clock_gettime(clock_id, &t)) { perror("clock_gettime"); exit(EXIT_FAILURE); }
    return (int64_t)t.tv_sec * INT64_C(1000000000) + t.tv_nsec;
}
static int64_t mono(void) { return now_ns(CLOCK_MONOTONIC); }
/* Liberacoes absolutas: o atraso de um job nao desloca os proximos periodos. */
static void sleep_until(int64_t target) {
    struct timespec t = {target / INT64_C(1000000000), target % INT64_C(1000000000)};
    int rc;
    do { rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL); } while (rc == EINTR);
    check(rc, "clock_nanosleep");
}
/* Carga computacional, em vez de nanosleep: usa CPU da propria thread.
 * O mesmo LCG da workload(70) da M2 e executado antes do complemento de carga.
 * O complemento ate 1 ms e uma carga sintetica do PC, nao WCET da ESP32. */
static void workload(int64_t cpu_ns) {
    int64_t start = now_ns(CLOCK_THREAD_CPUTIME_ID);
    volatile uint32_t value = 1;
    for (unsigned i = 0; i < 70U * 240U; ++i) value = value * 1664525U + 1013904223U;
    while (now_ns(CLOCK_THREAD_CPUTIME_ID) - start < cpu_ns)
        for (unsigned i = 0; i < 64; ++i) value = value * 1664525U + 1013904223U;
    (void)value;
}
static void record(record_t r) {
    check(pthread_mutex_lock(&log_mutex), "log lock");
    if (rec_count < MAX_RECORDS) records[rec_count++] = r;
    else ++rec_lost;
    check(pthread_mutex_unlock(&log_mutex), "log unlock");
}
static void enqueue(unsigned id) {
    job_t j = {id, mono()};
    check(pthread_mutex_lock(&queue_mutex), "queue lock");
    ++produced;
    if (depth == QUEUE_CAP) ++rejected;
    else {
        queue[tail] = j; tail = (tail + 1) % QUEUE_CAP;
        ++depth; ++accepted;
        if (depth > max_depth) max_depth = depth;
    }
    check(pthread_mutex_unlock(&queue_mutex), "queue unlock");
}
static int dequeue(job_t *j) {
    check(pthread_mutex_lock(&queue_mutex), "queue lock");
    int ok = depth != 0;
    if (ok) { *j = queue[head]; head = (head + 1) % QUEUE_CAP; --depth; }
    check(pthread_mutex_unlock(&queue_mutex), "queue unlock");
    return ok;
}
static unsigned queue_depth(void) {
    check(pthread_mutex_lock(&queue_mutex), "queue lock");
    unsigned result = depth;
    check(pthread_mutex_unlock(&queue_mutex), "queue unlock");
    return result;
}
/* SORT preserva a carga e o incremento de belt.production sob mutex da M2.
 * Somente o servidor chama esta funcao. Nao existe uma thread por pedido. */
static void sort_job(job_t j) {
    int64_t start = mono(), cpu_start = now_ns(CLOCK_THREAD_CPUTIME_ID);
    workload(NS_MS);
    check(pthread_mutex_lock(&belt_mutex), "belt lock");
    ++production;
    check(pthread_mutex_unlock(&belt_mutex), "belt unlock");
    int64_t end = mono(), cpu = now_ns(CLOCK_THREAD_CPUTIME_ID) - cpu_start;
    ++completed;
    record((record_t){'J',j.id,0,j.event,start,end,cpu,SORT_DEADLINE,queue_depth()});
}
static void *server_thread(void *arg) {
    (void)arg;
    unsigned activation = 0;
    for (int64_t release = epoch; release < stop_at; release += ts) {
        sleep_until(release);
        int64_t start = mono(), cpu_start = now_ns(CLOCK_THREAD_CPUTIME_ID);
        /* Mesmo servidor polling do exemplo: fila vazia perde o saldo.
         * Mede toda a janela de servico, incluindo fila e instrumentacao.
         * Nao inicia nova janela depois do fim do ensaio. */
        if (start >= stop_at) break;
        job_t j;
        while (mono() - start < cs && mono() < stop_at && dequeue(&j)) sort_job(j);
        int64_t end = mono(), cpu = now_ns(CLOCK_THREAD_CPUTIME_ID) - cpu_start;
        record((record_t){'S',++activation,0,release,start,end,cpu,cs,queue_depth()});
        /* Se houve atraso de um periodo inteiro, pula liberacoes vencidas:
         * nunca executa varios orcamentos acumulados em rajada. */
        int64_t now = mono();
        while (release + ts < now) release += ts;
    }
    return NULL;
}
static void *periodic_thread(void *arg) {
    periodic_t *p = arg;
    unsigned job = 0;
    int64_t period = p->period_ms * NS_MS;
    for (int64_t release = epoch; release < stop_at; release += period) {
        sleep_until(release);
        if (mono() >= stop_at) break;
        int64_t start = mono(), cpu_start = now_ns(CLOCK_THREAD_CPUTIME_ID);
        workload(5 * NS_MS);
        int64_t end = mono(), cpu = now_ns(CLOCK_THREAD_CPUTIME_ID) - cpu_start;
        record((record_t){'P',++job,p->id,release,start,end,cpu,period,0});
    }
    return NULL;
}
static void *generator_thread(void *arg) {
    (void)arg;
    uint32_t seed = 42; /* Mesma sequencia em todas as repeticoes. */
    unsigned id = 0;
    for (int64_t release = epoch + 10 * NS_MS; release < stop_at;) {
        sleep_until(release);
        if (mono() >= stop_at) break;
        unsigned batch = !strcmp(scenario,"empty") ? 0 : !strcmp(scenario,"burst") ? 12 :
                         !strcmp(scenario,"overload") ? 48 : 1;
        for (unsigned i = 0; i < batch; ++i) enqueue(++id);
        seed = seed * 1664525U + 1013904223U;
        int delay = !strcmp(scenario,"normal") ? 30 + (int)(seed % 90U) : 100;
        release += delay * NS_MS;
    }
    return NULL;
}
static void create(pthread_t *th, void *(*fn)(void *), void *arg, int priority) {
    pthread_attr_t attr;
    struct sched_param sp = {.sched_priority = priority};
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset); CPU_SET(cpu_core, &cpuset);
    check(pthread_attr_init(&attr), "attr_init");
    check(pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED), "explicit scheduling");
    check(pthread_attr_setschedpolicy(&attr,SCHED_FIFO), "SCHED_FIFO");
    check(pthread_attr_setschedparam(&attr,&sp), "priority");
    check(pthread_attr_setaffinity_np(&attr,sizeof(cpuset),&cpuset), "affinity");
    int rc = pthread_create(th,&attr,fn,arg);
    if (rc) fprintf(stderr,"Execute com sudo; nao ha fallback silencioso para SCHED_OTHER.\n");
    check(rc, "pthread_create");
    check(pthread_attr_destroy(&attr), "attr_destroy");
}
static long number(const char *s, long lo, long hi) {
    char *end; errno = 0; long v = strtol(s,&end,10);
    if (errno || !*s || *end || v < lo || v > hi) {
        fprintf(stderr,"Parametro invalido: %s (limite %ld..%ld)\n",s,lo,hi); exit(EXIT_FAILURE);
    }
    return v;
}
int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr,"Uso: %s empty|normal|burst|overload segundos Ts_ms Cs_ms\n",argv[0]); return 1;
    }
    scenario = argv[1];
    if (strcmp(scenario,"empty") && strcmp(scenario,"normal") && strcmp(scenario,"burst") && strcmp(scenario,"overload")) {
        fprintf(stderr,"Cenario invalido\n"); return 1;
    }
    int seconds = (int)number(argv[2],1,30);
    ts = number(argv[3],2,1000) * NS_MS; cs = number(argv[4],1,1000) * NS_MS;
    if (cs > ts) { fprintf(stderr,"Cs deve ser <= Ts\n"); return 1; }
    cpu_set_t available;
    if (sched_getaffinity(0,sizeof(available),&available)) { perror("sched_getaffinity"); return 1; }
    for (cpu_core = 0; cpu_core < CPU_SETSIZE && !CPU_ISSET(cpu_core,&available); ++cpu_core) {}
    if (cpu_core == CPU_SETSIZE) return 1;
    /* Epoch futura deixa todas as threads prontas antes da primeira liberacao. */
    epoch = mono() + 500 * NS_MS; stop_at = epoch + seconds * INT64_C(1000000000);
    printf("M3 SORT pthreads SCHED_FIFO cpu=%d scenario=%s seconds=%d Ts_ms=%" PRId64 " Cs_ms=%" PRId64 "\n",
           cpu_core,scenario,seconds,ts/NS_MS,cs/NS_MS);
    fflush(stdout);
    pthread_t server, tasks[4], generator;
    create(&server,server_thread,NULL,60);
    for (unsigned i = 0; i < 4; ++i) create(&tasks[i],periodic_thread,&periodic[i],periodic[i].priority);
    create(&generator,generator_thread,NULL,40);
    check(pthread_join(generator,NULL),"join generator");
    check(pthread_join(server,NULL),"join server");
    for (unsigned i = 0; i < 4; ++i) check(pthread_join(tasks[i],NULL),"join periodic");
    /* CSV impresso apenas depois dos joins: sem I/O no caminho medido. */
    puts("type,id,task,release_ns,start_ns,end_ns,cpu_ns,deadline_ns,depth");
    for (unsigned i = 0; i < rec_count; ++i) {
        record_t r = records[i];
        printf("%c,%u,%u,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%u\n",
               r.type,r.id,r.task,r.release-epoch,r.start-epoch,r.end-epoch,r.cpu,r.deadline,r.depth);
    }
    printf("TOTAL produced=%u accepted=%u rejected=%u completed=%u pending=%u production=%u max_depth=%u log_lost=%u\n",
           produced,accepted,rejected,completed,depth,production,max_depth,rec_lost);
    if (produced != accepted + rejected || accepted != completed + depth || production != completed || rec_lost) return 2;
    return 0;
}
