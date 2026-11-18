/*
 * etapa2_comportamental.c - Etapa 2 (escopo: COMPORTAMENTAL)
 * A tarefa Huffman responde a UM evento externo por vez: um processo
 * gerador separado (fork) envia um pacote UDP pela interface loopback
 * contendo o instante de emissao (CLOCK_MONOTONIC e comum aos processos).
 * O gerador so envia o proximo evento depois de receber o ACK do anterior
 * e de um intervalo aleatorio de 2 a 6 ms.
 *
 * Tempo de resposta = fim do processamento - instante de emissao do evento
 * (inclui pilha de rede, despertar da tarefa e a execucao do Huffman).
 *
 * Uso: etapa2 [-a ativacoes] [-n tamanho] [-s semente] [-o saida.csv] [-p porta]
 */
#include "common.h"
#include "huffman.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct { uint64_t seq; uint64_t t_send; } evt_t;

static void generator(size_t acts, int port, uint64_t seed)
{
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(4, &set);
    sched_setaffinity(0, sizeof set, &set);       /* gerador fora do nucleo RT */

    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
    uint64_t rs = rng_seed(seed ^ 0xABCDEFull);

    usleep(200000);                                /* deixa o receptor pronto */
    for (size_t k = 0; k < acts; k++) {
        uint64_t gap_us = 2000 + rng_next(&rs) % 4001;   /* 2..6 ms */
        usleep((useconds_t)gap_us);
        evt_t e = { k, now_ns() };
        sendto(sk, &e, sizeof e, 0, (struct sockaddr *)&dst, sizeof dst);
        evt_t ack;
        recv(sk, &ack, sizeof ack, 0);             /* um evento por vez */
    }
    close(sk);
    _exit(0);
}

int main(int argc, char **argv)
{
    size_t acts = 3000, n = DEF_N;
    uint64_t seed = DEF_SEED;
    int port = 50002;
    const char *out = "data/etapa2_comportamental.csv";
    int opt;
    while ((opt = getopt(argc, argv, "a:n:s:o:p:")) != -1) {
        switch (opt) {
        case 'a': acts = strtoull(optarg, NULL, 10); break;
        case 'n': n = strtoull(optarg, NULL, 10); break;
        case 's': seed = strtoull(optarg, NULL, 10); break;
        case 'o': out = optarg; break;
        case 'p': port = atoi(optarg); break;
        default: return 2;
        }
    }

    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_pton(AF_INET, "127.0.0.1", &me.sin_addr);
    if (bind(sk, (struct sockaddr *)&me, sizeof me) < 0) { perror("bind"); return 1; }

    pid_t pid = fork();
    if (pid == 0) { close(sk); generator(acts, port, seed); }

    uint8_t *shared = malloc(n), *local = malloc(n), *enc = malloc(8 * n + 8), *dec = malloc(n);
    uint64_t *resp = calloc(acts, sizeof *resp);
    if (!shared || !local || !enc || !dec || !resp) { perror("malloc"); return 1; }

    lock_memory();
    rt_pin_and_prio(RT_CPU, 80, "etapa2");

    huff_ctx_t ctx;
    huff_setup(&ctx, enc, dec);
    uint64_t rs = rng_seed(seed);
    size_t fails = 0;

    for (size_t k = 0; k < acts; k++) {
        fill_random(shared, n, &rs);           /* dado pronto antes do evento */
        evt_t e;
        struct sockaddr_in from; socklen_t fl = sizeof from;
        ssize_t r = recvfrom(sk, &e, sizeof e, 0, (struct sockaddr *)&from, &fl);
        if (r != (ssize_t)sizeof e) { perror("recvfrom"); return 1; }
        memcpy(local, shared, n);
        int ok = huff_task(&ctx, local, n);
        uint64_t t1 = now_ns();
        resp[k] = t1 - e.t_send;
        fails += (ok != 0);
        sendto(sk, &e, sizeof e, 0, (struct sockaddr *)&from, fl);   /* ACK */
    }
    waitpid(pid, NULL, 0);

    printf("etapa2: %zu eventos, N=%zu, semente=%llu, falhas de validacao=%zu\n",
           acts, n, (unsigned long long)seed, fails);
    write_csv(out, resp, acts);
    return fails ? 1 : 0;
}
