/*
 * huffman.h - Codificacao de Huffman (tarefa sob teste)
 * Sistemas em Tempo Real - UNIVALI - Atividade complementar M3
 *
 * Modulo puramente computacional: nao aloca memoria (os buffers sao
 * fornecidos pelo chamador), para que o tempo medido seja somente o do
 * algoritmo e para que todos os desvios sejam alcancaveis por teste.
 */
#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stddef.h>
#include <stdint.h>

#define HUFF_SYMBOLS 256
#define HUFF_NODES   (2 * HUFF_SYMBOLS - 1)

typedef struct {
    uint64_t freq;
    int16_t  left;    /* -1 => folha */
    int16_t  right;
    int16_t  symbol;  /* valido apenas em folhas */
} huff_node_t;

typedef struct {
    uint64_t    freq[HUFF_SYMBOLS];
    huff_node_t node[HUFF_NODES];
    int         n_nodes;
    int         root;                 /* -1 => arvore vazia */
    int         heap[HUFF_SYMBOLS];
    int         heap_size;
    uint64_t    code[HUFF_SYMBOLS];   /* codigo (bits menos significativos) */
    uint8_t     code_len[HUFF_SYMBOLS];
    uint8_t    *enc;                  /* saida codificada  (>= 8*n bytes) */
    size_t      enc_bits;
    uint8_t    *dec;                  /* saida decodificada (>= n bytes)  */
} huff_ctx_t;

/* Associa os buffers de trabalho ao contexto.
 * enc deve ter pelo menos 8*n_max bytes (pior caso: codigos de 64 bits)
 * dec deve ter pelo menos n_max bytes. */
void huff_setup(huff_ctx_t *c, uint8_t *enc, uint8_t *dec);

/* Constroi a arvore e codifica in[0..n-1] em c->enc. */
void huff_encode(huff_ctx_t *c, const uint8_t *in, size_t n);

/* Decodifica c->enc (n simbolos) em c->dec. */
void huff_decode(huff_ctx_t *c, size_t n);

/* Compara dois buffers. 0 = iguais, -1 = diferentes. */
int huff_validate(const uint8_t *a, const uint8_t *b, size_t n);

/* Tarefa completa: codifica, decodifica e valida. 0 = saida correta. */
int huff_task(huff_ctx_t *c, const uint8_t *in, size_t n);

#endif
