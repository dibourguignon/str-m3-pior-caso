/*
 * test_huffman.c - Casos de teste da tarefa Huffman (Experimento A)
 *
 * Executa os casos 1..K (K = argv[1], padrao = todos). Cada caso roda a
 * tarefa completa (codifica, decodifica, valida) e confere o resultado
 * esperado. Imprime CSV: caso,descricao,n,repeticoes,ok,hwm_us
 *
 * O script de cobertura roda K = 1, 2, ..., total para mostrar como a
 * cobertura de linha/desvio cresce a cada caso adicionado.
 */
#include "common.h"
#include "huffman.h"

#ifndef NMAX
#define NMAX 32768u        /* Linux; o ESP32 usa NMAX=1024 (scripts/coverage.sh testa os dois) */
#endif
#ifndef FIB_N
#define FIB_N 17710u
#endif
static uint8_t in[NMAX], enc[8 * NMAX + 8], dec[NMAX];
static huff_ctx_t ctx;

typedef struct { const char *desc; size_t n; void (*gen)(uint8_t *, size_t); int expect; } tcase_t;

static void gen_random(uint8_t *b, size_t n)   { uint64_t s = rng_seed(DEF_SEED); fill_random(b, n, &s); }
static void gen_one(uint8_t *b, size_t n)      { memset(b, 'A', n); }
static void gen_two(uint8_t *b, size_t n)      { for (size_t i = 0; i < n; i++) b[i] = (i % 3) ? 'x' : 'y'; }
static void gen_all256(uint8_t *b, size_t n)   { for (size_t i = 0; i < n; i++) b[i] = (uint8_t)i; }
static void gen_fib(uint8_t *b, size_t n)
{   /* frequencias de Fibonacci -> arvore degenerada (codigos longos) */
    size_t f[20] = {1, 1}, pos = 0;
    for (int i = 2; i < 20; i++) f[i] = f[i - 1] + f[i - 2];
    for (int s = 0; s < 20 && pos < n; s++)
        for (size_t k = 0; k < f[s] && pos < n; k++) b[pos++] = (uint8_t)('a' + s);
    while (pos < n) b[pos++] = 'a';
}
static void gen_empty(uint8_t *b, size_t n)    { (void)b; (void)n; }

static const tcase_t cases[] = {
    { "aleatorio N=NMAX (mesma entrada da etapa 1)",  NMAX,  gen_random, 0 },
    { "entrada vazia (n=0)",                          0,     gen_empty,  0 },
    { "um unico simbolo repetido (n=1000)",           1000,  gen_one,    0 },
    { "dois simbolos (n=999)",                        999,   gen_two,    0 },
    { "256 simbolos com frequencias iguais",          256,   gen_all256, 0 },
    { "frequencias de Fibonacci (arvore degenerada)", FIB_N, gen_fib,    0 },
    { "validacao detecta saida corrompida",           64,    gen_random, -1 },
};
#define NCASES (sizeof cases / sizeof cases[0])

int main(int argc, char **argv)
{
    size_t upto = argc > 1 ? strtoull(argv[1], NULL, 10) : NCASES;
    int reps = argc > 2 ? atoi(argv[2]) : 200;
    if (upto > NCASES) upto = NCASES;
    huff_setup(&ctx, enc, dec);
    int failures = 0;
    printf("caso,descricao,n,repeticoes,ok,hwm_us\n");
    for (size_t c = 0; c < upto; c++) {
        const tcase_t *t = &cases[c];
        t->gen(in, t->n);
        uint64_t hwm = 0;
        int got = 0;
        for (int r = 0; r < reps; r++) {
            uint64_t t0 = now_ns();
            if (t->expect == 0) {
                got = huff_task(&ctx, in, t->n);
            } else {
                huff_encode(&ctx, in, t->n);
                huff_decode(&ctx, t->n);
                ctx.dec[t->n / 2] ^= 0x01;          /* corrompe 1 bit da saida */
                got = huff_validate(in, ctx.dec, t->n);
            }
            uint64_t dt = now_ns() - t0;
            if (dt > hwm) hwm = dt;
        }
        int ok = (got == t->expect);
        failures += !ok;
        printf("%zu,%s,%zu,%d,%s,%.3f\n", c + 1, t->desc, t->n, reps, ok ? "sim" : "NAO", hwm / 1000.0);
    }
    return failures ? 1 : 0;
}
