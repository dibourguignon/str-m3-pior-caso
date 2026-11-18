/*
 * etapa1_tarefa.c - Etapa 1 (escopo: TAREFA)
 * A tarefa Huffman isolada, sem estresse. Cada ativacao:
 *   (fora da medicao) gera um novo array aleatorio a partir da semente
 *   t0 -> copia de entrada + codifica + decodifica + valida -> t1
 * Tempo de resposta = t1 - t0.
 *
 * Uso: etapa1 [-a ativacoes] [-n tamanho] [-s semente] [-o saida.csv]
 */
#include "common.h"
#include "huffman.h"
#include <unistd.h>

int main(int argc, char **argv)
{
    size_t acts = 10000, n = DEF_N;
    uint64_t seed = DEF_SEED;
    const char *out = "data/etapa1_tarefa.csv";
    int opt;
    while ((opt = getopt(argc, argv, "a:n:s:o:")) != -1) {
        switch (opt) {
        case 'a': acts = strtoull(optarg, NULL, 10); break;
        case 'n': n = strtoull(optarg, NULL, 10); break;
        case 's': seed = strtoull(optarg, NULL, 10); break;
        case 'o': out = optarg; break;
        default: fprintf(stderr, "uso: %s [-a acts] [-n N] [-s seed] [-o csv]\n", argv[0]); return 2;
        }
    }

    uint8_t *shared = malloc(n), *local = malloc(n), *enc = malloc(8 * n + 8), *dec = malloc(n);
    uint64_t *resp = calloc(acts, sizeof *resp);
    if (!shared || !local || !enc || !dec || !resp) { perror("malloc"); return 1; }

    lock_memory();
    rt_pin_and_prio(RT_CPU, 80, "etapa1");

    huff_ctx_t ctx;
    huff_setup(&ctx, enc, dec);
    uint64_t rs = rng_seed(seed);
    size_t fails = 0;

    for (size_t k = 0; k < acts; k++) {
        fill_random(shared, n, &rs);           /* preparacao: fora da medicao */
        uint64_t t0 = now_ns();
        memcpy(local, shared, n);              /* mesma copia feita nas etapas 3/4 */
        int r = huff_task(&ctx, local, n);
        uint64_t t1 = now_ns();
        resp[k] = t1 - t0;
        fails += (r != 0);
    }

    printf("etapa1: %zu ativacoes, N=%zu, semente=%llu, falhas de validacao=%zu, bits codificados (ultima)=%zu (%.2f bits/simbolo)\n",
           acts, n, (unsigned long long)seed, fails, ctx.enc_bits, (double)ctx.enc_bits / (double)n);
    write_csv(out, resp, acts);
    return fails ? 1 : 0;
}
