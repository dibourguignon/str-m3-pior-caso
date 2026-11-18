/*
 * huffman.c - Codificacao de Huffman com validacao (decodifica e compara)
 * Sistemas em Tempo Real - UNIVALI - Atividade complementar M3
 */
#include "huffman.h"
#include <string.h>

void huff_setup(huff_ctx_t *c, uint8_t *enc, uint8_t *dec)
{
    c->enc = enc;
    c->dec = dec;
    c->enc_bits = 0;
    c->root = -1;
    c->n_nodes = 0;
    c->heap_size = 0;
}

/* ---------------- min-heap de indices de nos ---------------- */

/* Ordem total: frequencia e, em empate, indice do no (determinismo). */
static int node_less(const huff_ctx_t *c, int a, int b)
{
    if (c->node[a].freq != c->node[b].freq)
        return c->node[a].freq < c->node[b].freq;
    return a < b;
}

static void heap_swap(huff_ctx_t *c, int i, int j)
{
    int t = c->heap[i];
    c->heap[i] = c->heap[j];
    c->heap[j] = t;
}

static void heap_up(huff_ctx_t *c, int i)
{
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!node_less(c, c->heap[i], c->heap[p]))
            break;
        heap_swap(c, i, p);
        i = p;
    }
}

static void heap_down(huff_ctx_t *c, int i)
{
    for (;;) {
        int l = 2 * i + 1;
        int r = l + 1;
        int m = i;
        if (l < c->heap_size && node_less(c, c->heap[l], c->heap[m]))
            m = l;
        if (r < c->heap_size && node_less(c, c->heap[r], c->heap[m]))
            m = r;
        if (m == i)
            break;
        heap_swap(c, i, m);
        i = m;
    }
}

static void heap_push(huff_ctx_t *c, int idx)
{
    c->heap[c->heap_size] = idx;
    heap_up(c, c->heap_size);
    c->heap_size++;
}

static int heap_pop(huff_ctx_t *c)
{
    int top = c->heap[0];
    c->heap_size--;
    c->heap[0] = c->heap[c->heap_size];
    heap_down(c, 0);
    return top;
}

/* ---------------- arvore e codigos ---------------- */

static void build_tree(huff_ctx_t *c)
{
    c->n_nodes = 0;
    c->heap_size = 0;
    for (int s = 0; s < HUFF_SYMBOLS; s++) {
        if (c->freq[s] == 0)
            continue;
        huff_node_t *nd = &c->node[c->n_nodes];
        nd->freq = c->freq[s];
        nd->left = nd->right = -1;
        nd->symbol = (int16_t)s;
        heap_push(c, c->n_nodes);
        c->n_nodes++;
    }
    if (c->heap_size == 0) {          /* entrada vazia */
        c->root = -1;
        return;
    }
    while (c->heap_size > 1) {
        int a = heap_pop(c);
        int b = heap_pop(c);
        huff_node_t *nd = &c->node[c->n_nodes];
        nd->freq = c->node[a].freq + c->node[b].freq;
        nd->left = (int16_t)a;
        nd->right = (int16_t)b;
        nd->symbol = -1;
        heap_push(c, c->n_nodes);
        c->n_nodes++;
    }
    c->root = heap_pop(c);
}

static void gen_codes(huff_ctx_t *c)
{
    memset(c->code_len, 0, sizeof c->code_len);
    if (c->node[c->root].left < 0) {  /* um unico simbolo: codigo "0" */
        c->code[c->node[c->root].symbol] = 0;
        c->code_len[c->node[c->root].symbol] = 1;
        return;
    }
    /* DFS iterativa: pilha de (no, codigo, comprimento) */
    int      st_node[HUFF_NODES];
    uint64_t st_code[HUFF_NODES];
    uint8_t  st_len[HUFF_NODES];
    int sp = 0;
    st_node[sp] = c->root; st_code[sp] = 0; st_len[sp] = 0; sp++;
    while (sp > 0) {
        sp--;
        int n = st_node[sp];
        uint64_t code = st_code[sp];
        uint8_t len = st_len[sp];
        const huff_node_t *nd = &c->node[n];
        if (nd->left < 0) {
            c->code[nd->symbol] = code;
            c->code_len[nd->symbol] = len;
            continue;
        }
        st_node[sp] = nd->left;  st_code[sp] = code << 1;        st_len[sp] = (uint8_t)(len + 1); sp++;
        st_node[sp] = nd->right; st_code[sp] = (code << 1) | 1u; st_len[sp] = (uint8_t)(len + 1); sp++;
    }
}

void huff_encode(huff_ctx_t *c, const uint8_t *in, size_t n)
{
    memset(c->freq, 0, sizeof c->freq);
    for (size_t i = 0; i < n; i++)
        c->freq[in[i]]++;

    build_tree(c);
    c->enc_bits = 0;
    if (c->root < 0)
        return;
    gen_codes(c);

    size_t total_bits = 0;
    for (int s = 0; s < HUFF_SYMBOLS; s++)
        total_bits += (size_t)c->freq[s] * c->code_len[s];
    memset(c->enc, 0, (total_bits + 7) / 8);

    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t code = c->code[in[i]];
        int len = c->code_len[in[i]];
        for (int b = len - 1; b >= 0; b--) {
            if ((code >> b) & 1u)
                c->enc[pos >> 3] |= (uint8_t)(0x80u >> (pos & 7));
            pos++;
        }
    }
    c->enc_bits = pos;
}

void huff_decode(huff_ctx_t *c, size_t n)
{
    if (c->root < 0)
        return;
    const huff_node_t *root = &c->node[c->root];
    if (root->left < 0) {             /* um unico simbolo */
        for (size_t i = 0; i < n; i++)
            c->dec[i] = (uint8_t)root->symbol;
        return;
    }
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        int idx = c->root;
        while (c->node[idx].left >= 0) {
            int bit = (c->enc[pos >> 3] >> (7 - (pos & 7))) & 1;
            pos++;
            idx = bit ? c->node[idx].right : c->node[idx].left;
        }
        c->dec[i] = (uint8_t)c->node[idx].symbol;
    }
}

int huff_validate(const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return -1;
    return 0;
}

int huff_task(huff_ctx_t *c, const uint8_t *in, size_t n)
{
    huff_encode(c, in, n);
    huff_decode(c, n);
    return huff_validate(in, c->dec, n);
}
