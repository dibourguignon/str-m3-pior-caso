/*
 * huffman.c - Codificacao de Huffman com validacao (decodifica e compara)
 * Sistemas em Tempo Real - UNIVALI - Atividade complementar M3
 *
 * Uma ativacao da tarefa (huff_task) faz, em ordem:
 *   1. conta quantas vezes cada byte aparece na entrada (freq[]);
 *   2. monta a arvore de Huffman juntando sempre os dois nos de menor
 *      frequencia (min-heap) -> simbolos frequentes ficam perto da raiz;
 *   3. percorre a arvore e gera o codigo de cada simbolo
 *      (esquerda = bit 0, direita = bit 1);
 *   4. codifica a entrada, bit a bit, em c->enc;
 *   5. decodifica c->enc descendo a arvore a partir da raiz;
 *   6. compara a saida decodificada com a entrada (validacao exigida pelo
 *      enunciado: o teste de tarefa confirma a corretude logica).
 *
 * O modulo nao usa malloc (os buffers vem do chamador): o tempo medido e so o
 * do algoritmo, sem variacao do alocador, e todos os desvios podem ser
 * alcancados pelos 7 casos de teste (100% de cobertura de linha e de desvio).
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
/* Fila de prioridade em vetor: o no de menor frequencia fica sempre em heap[0].
 * Filhos de i: 2i+1 e 2i+2; pai de i: (i-1)/2. Inserir e remover custam O(log n). */

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

/* Monta a arvore de Huffman. Os nos ficam no vetor c->node (sem malloc):
 * primeiro uma folha por simbolo presente, depois os nos internos criados
 * ao juntar os dois menores. Com k simbolos distintos ha 2k-1 nos. */
static void build_tree(huff_ctx_t *c)
{
    c->n_nodes = 0;
    c->heap_size = 0;
    /* uma folha para cada simbolo que aparece pelo menos uma vez */
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
    /* passo central do algoritmo: retira os dois nos menos frequentes e cria
     * um no pai com a soma das frequencias, ate sobrar um so (a raiz) */
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

/* Gera o codigo de cada simbolo: o caminho da raiz ate a folha
 * (esquerda = 0, direita = 1). A profundidade da folha e o tamanho do codigo. */
static void gen_codes(huff_ctx_t *c)
{
    memset(c->code_len, 0, sizeof c->code_len);
    /* caso especial: so um simbolo distinto -> a raiz ja e folha (caminho vazio);
     * usa-se o codigo "0" com 1 bit para que a codificacao tenha tamanho > 0 */
    if (c->node[c->root].left < 0) {
        c->code[c->node[c->root].symbol] = 0;
        c->code_len[c->node[c->root].symbol] = 1;
        return;
    }
    /* busca em profundidade iterativa (sem recursao, pilha de tamanho fixo):
     * cada entrada guarda o no, o codigo acumulado ate ele e seu comprimento */
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
    /* 1. histograma de frequencias */
    memset(c->freq, 0, sizeof c->freq);
    for (size_t i = 0; i < n; i++)
        c->freq[in[i]]++;

    /* 2-3. arvore e codigos (entrada vazia: nao ha o que codificar) */
    build_tree(c);
    c->enc_bits = 0;
    if (c->root < 0)
        return;
    gen_codes(c);

    /* 4. escreve os codigos em sequencia, bit a bit, do mais significativo
     *    para o menos significativo de cada byte de saida */
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

/* 5. Decodifica: para cada simbolo, parte da raiz e segue um bit por vez
 *    (0 = esquerda, 1 = direita) ate chegar a uma folha. Como nenhum codigo e
 *    prefixo de outro, a folha encontrada identifica o simbolo sem ambiguidade. */
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

/* 6. Validacao: a saida decodificada tem de ser identica a entrada.
 *    O caso de teste 7 corrompe 1 bit de proposito para exercitar o retorno -1. */
int huff_validate(const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return -1;
    return 0;
}

/* Uma ativacao completa da tarefa sob teste. E exatamente este trecho que fica
 * entre as duas leituras de clock_gettime nas etapas 1 a 4. */
int huff_task(huff_ctx_t *c, const uint8_t *in, size_t n)
{
    huff_encode(c, in, n);
    huff_decode(c, n);
    return huff_validate(in, c->dec, n);
}
