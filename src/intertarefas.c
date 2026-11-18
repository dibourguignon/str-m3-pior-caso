/*
 * intertarefas.c - Etapas 3 (INTERTAREFAS) e 4 (SISTEMA)
 *
 * Tres tarefas periodicas no MESMO nucleo (RT_CPU), SCHED_FIFO, prioridades
 * em ordem rate-monotonic, compartilhando um buffer protegido por mutex com
 * heranca de prioridade (PTHREAD_PRIO_INHERIT):
 *
 *   sensor   T=4 ms   prio 90  ~250 us de processamento + escreve 4 KiB no buffer (mutex)
 *   huffman  T=10 ms  prio 80  copia o buffer de N bytes (mutex) + Huffman   <- tarefa medida
 *   logger   T=33 ms  prio 70  segura o mutex ~1,5 ms (checksum + "flush" do log)
 *
 * Taxas de dados diferentes: sensor produz 1 KiB/ms, huffman consome N/10 ms,
 * logger le o buffer inteiro a cada 33 ms.
 *
 * Etapa 4 (-m 4) acrescenta o escopo comportamental: um processo externo envia
 * eventos UDP (um por vez, intervalo aleatorio 1..5 ms) tratados por uma tarefa
 * esporadica (prio 85) que codifica um pacote de 8 KiB com Huffman. A carga
 * externa (stress-ng) e iniciada pelo script run_all.sh.
 *
 * Tempo de resposta do huffman = fim do job - instante de liberacao periodica.
 *
 * Uso: intertarefas -m 3|4 [-a ativacoes] [-n N] [-s semente] [-o csv] [-d csv_detalhe]
 */
#include "common.h"
#include "huffman.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define SENSOR_T_NS    4000000ull
#define HUFF_T_NS     10000000ull
#define LOGGER_T_NS   33000000ull
/* trabalho sintetico em numero FIXO de iteracoes (calibrado uma vez, sem carga:
 * ~975 iteracoes/us), para que a quantidade de trabalho seja a mesma nas etapas
 * 3 e 4 - sob estresse ele simplesmente demora mais, como codigo real. */
#define SENSOR_WORK_ITERS   244000ull     /* ~250 us sem carga  */
#define LOGGER_CS_ITERS    1460000ull     /* ~1,5 ms sem carga  */
#define CHUNK           4096u
#define EVT_BYTES       8192u
#define EVT_PORT        50004

static size_t   g_n = DEF_N;
static size_t   g_acts = 10000;
static uint64_t g_seed = DEF_SEED;
static int      g_mode = 3;
static uint64_t g_t0;                     /* instante critico: todas liberadas juntas */
static atomic_int g_stop;

static pthread_mutex_t g_mtx;
static uint8_t *g_buf;                    /* buffer compartilhado (N bytes) */
static size_t   g_wpos;
static uint64_t g_sensor_rs;

static uint64_t *g_resp, *g_lat, *g_wait;
static size_t    g_fails;
static size_t    g_evt_count;

/* ------------------------------------------------------------------ */
static void *sensor_thread(void *arg)
{
    (void)arg;
    rt_pin_and_prio(RT_CPU, 90, "sensor");
    for (uint64_t k = 0; !atomic_load(&g_stop); k++) {
        sleep_until_ns(g_t0 + k * SENSOR_T_NS);
        busy_iters(SENSOR_WORK_ITERS);                      /* aquisicao/filtragem */
        pthread_mutex_lock(&g_mtx);
        size_t len = CHUNK;
        if (g_wpos + len > g_n) len = g_n - g_wpos;
        fill_random(g_buf + g_wpos, len, &g_sensor_rs);
        g_wpos = (g_wpos + len) % g_n;
        pthread_mutex_unlock(&g_mtx);
    }
    return NULL;
}

static void *huffman_thread(void *arg)
{
    (void)arg;
    rt_pin_and_prio(RT_CPU, 80, "huffman");
    uint8_t *local = malloc(g_n), *enc = malloc(8 * g_n + 8), *dec = malloc(g_n);
    memset(local, 0, g_n); memset(enc, 0, 8 * g_n + 8); memset(dec, 0, g_n);
    huff_ctx_t ctx;
    huff_setup(&ctx, enc, dec);
    for (size_t k = 0; k < g_acts; k++) {
        uint64_t rel = g_t0 + k * HUFF_T_NS;
        sleep_until_ns(rel);
        uint64_t ts = now_ns();
        pthread_mutex_lock(&g_mtx);
        uint64_t tl = now_ns();
        memcpy(local, g_buf, g_n);
        pthread_mutex_unlock(&g_mtx);
        int r = huff_task(&ctx, local, g_n);
        uint64_t te = now_ns();
        g_resp[k] = te - rel;
        g_lat[k]  = ts > rel ? ts - rel : 0;
        g_wait[k] = tl - ts;
        g_fails += (r != 0);
    }
    atomic_store(&g_stop, 1);
    return NULL;
}

static void *logger_thread(void *arg)
{
    (void)arg;
    rt_pin_and_prio(RT_CPU, 70, "logger");
    for (uint64_t k = 0; !atomic_load(&g_stop); k++) {
        sleep_until_ns(g_t0 + k * LOGGER_T_NS);
        pthread_mutex_lock(&g_mtx);
        uint32_t sum = 0;
        for (size_t i = 0; i < g_n; i++) sum = sum * 31u + g_buf[i];
        g_sink ^= sum;
        busy_iters(LOGGER_CS_ITERS);                        /* "flush" do log */
        pthread_mutex_unlock(&g_mtx);
    }
    return NULL;
}

/* ---------------- etapa 4: evento externo esporadico ---------------- */
typedef struct { uint64_t seq; uint64_t t_send; } evt_t;

static void generator(uint64_t seed)
{
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(4, &set);
    sched_setaffinity(0, sizeof set, &set);
    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval tv = { 0, 200000 };
    setsockopt(sk, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons(EVT_PORT) };
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
    uint64_t rs = rng_seed(seed ^ 0x5151ull);
    for (uint64_t k = 0;; k++) {
        usleep((useconds_t)(1000 + rng_next(&rs) % 4001));   /* 1..5 ms */
        evt_t e = { k, now_ns() };
        if (sendto(sk, &e, sizeof e, 0, (struct sockaddr *)&dst, sizeof dst) < 0) _exit(0);
        evt_t ack;
        recv(sk, &ack, sizeof ack, 0);                       /* um evento por vez */
    }
}

static int g_evt_sock = -1;

static void *event_thread(void *arg)
{
    (void)arg;
    rt_pin_and_prio(RT_CPU, 85, "evento");
    uint8_t *pkt = malloc(EVT_BYTES), *enc = malloc(8 * EVT_BYTES + 8), *dec = malloc(EVT_BYTES);
    huff_ctx_t ctx;
    huff_setup(&ctx, enc, dec);
    uint64_t rs = rng_seed(g_seed ^ 0x777ull);
    while (!atomic_load(&g_stop)) {
        evt_t e;
        struct sockaddr_in from; socklen_t fl = sizeof from;
        ssize_t r = recvfrom(g_evt_sock, &e, sizeof e, 0, (struct sockaddr *)&from, &fl);
        if (r != (ssize_t)sizeof e) continue;                 /* timeout */
        fill_random(pkt, EVT_BYTES, &rs);                     /* "payload" do pacote */
        g_fails += (huff_task(&ctx, pkt, EVT_BYTES) != 0);
        g_evt_count++;
        sendto(g_evt_sock, &e, sizeof e, 0, (struct sockaddr *)&from, fl);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    const char *out = NULL, *detail = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "m:a:n:s:o:d:")) != -1) {
        switch (opt) {
        case 'm': g_mode = atoi(optarg); break;
        case 'a': g_acts = strtoull(optarg, NULL, 10); break;
        case 'n': g_n = strtoull(optarg, NULL, 10); break;
        case 's': g_seed = strtoull(optarg, NULL, 10); break;
        case 'o': out = optarg; break;
        case 'd': detail = optarg; break;
        default: return 2;
        }
    }
    if (!out) out = g_mode == 4 ? "data/etapa4_sistema.csv" : "data/etapa3_intertarefas.csv";

    pid_t gen = -1;
    if (g_mode == 4) {
        g_evt_sock = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(EVT_PORT) };
        inet_pton(AF_INET, "127.0.0.1", &me.sin_addr);
        if (bind(g_evt_sock, (struct sockaddr *)&me, sizeof me) < 0) { perror("bind"); return 1; }
        struct timeval tv = { 0, 100000 };
        setsockopt(g_evt_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        gen = fork();
        if (gen == 0) { close(g_evt_sock); generator(g_seed); }
    }

    g_buf  = malloc(g_n);
    g_resp = calloc(g_acts, sizeof *g_resp);
    g_lat  = calloc(g_acts, sizeof *g_lat);
    g_wait = calloc(g_acts, sizeof *g_wait);
    if (!g_buf || !g_resp || !g_lat || !g_wait) { perror("malloc"); return 1; }
    g_sensor_rs = rng_seed(g_seed);
    fill_random(g_buf, g_n, &g_sensor_rs);

    lock_memory();
    rt_pin_and_prio(RT_CPU, 0, "main");
    busy_calibrate();

    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&g_mtx, &ma);

    /* main fica fora do nucleo RT enquanto espera */
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(0, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);

    g_t0 = now_ns() + 200000000ull;          /* 200 ms para todos estarem prontos */
    pthread_t ts, th, tl, te;
    pthread_create(&ts, NULL, sensor_thread, NULL);
    pthread_create(&th, NULL, huffman_thread, NULL);
    pthread_create(&tl, NULL, logger_thread, NULL);
    if (g_mode == 4) pthread_create(&te, NULL, event_thread, NULL);

    pthread_join(th, NULL);
    pthread_join(ts, NULL);
    pthread_join(tl, NULL);
    if (g_mode == 4) {
        pthread_join(te, NULL);
        kill(gen, SIGTERM);
        waitpid(gen, NULL, 0);
    }

    printf("etapa%d: %zu ativacoes, N=%zu, semente=%llu, falhas=%zu, eventos tratados=%zu, iters/us=%.1f\n",
           g_mode, g_acts, g_n, (unsigned long long)g_seed, g_fails, g_evt_count, g_iters_per_us);
    write_csv(out, g_resp, g_acts);
    if (detail) {
        FILE *f = fopen(detail, "w");
        if (f) {
            fprintf(f, "ativacao,tempo_us,latencia_inicio_us,espera_mutex_us\n");
            for (size_t i = 0; i < g_acts; i++)
                fprintf(f, "%zu,%.3f,%.3f,%.3f\n", i + 1, g_resp[i] / 1000.0, g_lat[i] / 1000.0, g_wait[i] / 1000.0);
            fclose(f);
        }
    }
    return g_fails ? 1 : 0;
}
