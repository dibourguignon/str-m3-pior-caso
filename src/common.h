/*
 * common.h - Utilitarios comuns: relogio, PRNG, configuracao RT, CSV
 * Sistemas em Tempo Real - UNIVALI - Atividade complementar M3
 */
#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

/* ---- Parametros padrao (registrados no relatorio) ---- */
#define DEF_N        32768u      /* tamanho do array de caracteres       */
#define DEF_SEED     20261118u   /* semente do gerador aleatorio         */
#define RT_CPU       2           /* nucleo onde as tarefas RT executam   */

/* ---- relogio ---- */
static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline struct timespec ns_to_ts(uint64_t ns)
{
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    return ts;
}

static inline void sleep_until_ns(uint64_t t)
{
    struct timespec ts = ns_to_ts(t);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

/* ---- PRNG xorshift64* (reprodutivel, independente da libc) ---- */
static inline uint64_t rng_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static inline uint64_t rng_seed(uint64_t seed)
{
    uint64_t s = seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
    return s ? s : 1;
}

/* Caracteres ASCII imprimiveis (32..126) com distribuicao nao uniforme:
 * o minimo de dois sorteios favorece simbolos baixos -> arvore de Huffman
 * com codigos de comprimentos variados (caso mais interessante que o uniforme). */
static inline void fill_random(uint8_t *buf, size_t n, uint64_t *s)
{
    for (size_t i = 0; i < n; i++) {
        uint64_t r = rng_next(s);
        uint32_t a = (uint32_t)(r % 95u), b = (uint32_t)((r >> 32) % 95u);
        buf[i] = (uint8_t)(32u + (a < b ? a : b));
    }
}

/* ---- configuracao de tempo real ---- */
static inline void rt_pin_and_prio(int cpu, int prio, const char *who)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    int e = pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    if (e) fprintf(stderr, "[%s] aviso: afinidade falhou: %s\n", who, strerror(e));
    if (prio > 0) {
        struct sched_param sp = { .sched_priority = prio };
        e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (e) fprintf(stderr, "[%s] aviso: SCHED_FIFO falhou (%s) - rode como root\n", who, strerror(e));
    }
}

static inline void lock_memory(void)
{
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        perror("aviso: mlockall");
}

/* ---- trabalho sintetico calibrado (CPU pura, sem depender do relogio) ---- */
static volatile uint64_t g_sink;
static double g_iters_per_us = 0;

static inline void busy_iters(uint64_t iters)
{
    uint64_t x = 88172645463325252ull;
    for (uint64_t i = 0; i < iters; i++)
        x = x * 6364136223846793005ull + 1442695040888963407ull;
    g_sink = x;
}

static inline void busy_calibrate(void)
{
    const uint64_t it = 20000000ull;
    busy_iters(it / 10);                    /* aquece */
    uint64_t best = UINT64_MAX;
    for (int k = 0; k < 5; k++) {
        uint64_t t0 = now_ns();
        busy_iters(it);
        uint64_t dt = now_ns() - t0;
        if (dt < best) best = dt;
    }
    g_iters_per_us = (double)it / ((double)best / 1000.0);
}

static inline void busy_us(double us)
{
    busy_iters((uint64_t)(us * g_iters_per_us));
}

/* ---- CSV ---- */
static inline int write_csv(const char *path, const uint64_t *resp_ns, size_t count)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return -1; }
    fprintf(f, "ativacao,tempo_us\n");
    for (size_t i = 0; i < count; i++)
        fprintf(f, "%zu,%.3f\n", i + 1, (double)resp_ns[i] / 1000.0);
    fclose(f);
    return 0;
}

#endif
