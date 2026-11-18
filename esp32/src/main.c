/*
 * main.c - Plataforma principal: ESP32 (Xtensa LX6 dual-core 240 MHz, FreeRTOS SMP, ESP-IDF 6.0.1)
 * Sistemas em Tempo Real - UNIVALI - Atividade complementar M3
 *
 * Tarefa sob teste: src/huffman.c (o mesmo codigo usado na plataforma de comparacao Linux).
 * As tarefas sob teste executam no nucleo 1 (APP_CPU); o nucleo 0 (PRO_CPU) fica com o
 * sistema e com o gerador de eventos.
 *
 *   Casos    Experimento A   7 casos de teste (mesmos de tests/test_huffman.c), HWM de cada um
 *   Etapa 1  Tarefa          Huffman isolado (prio 10)
 *   Etapa 2  Comportamental  interrupcao de GPIO disparada pelo nucleo 0, um evento por vez
 *   Etapa 3  Intertarefas    sensor/huffman/logger liberados por timer de hardware (gptimer 1 kHz),
 *                            mutex FreeRTOS (heranca de prioridade)
 *   Etapa 4  Sistema         etapa 3 + eventos GPIO esporadicos + rajada UART vinda do PC
 *                            (quadros deterministicos enviados por esp32/capture.py)
 *
 * Tempo: clock_gettime(CLOCK_MONOTONIC) (resolucao de 1 us no ESP-IDF).
 * Saida: blocos CSV pela UART0 (115200 baud) capturados por esp32/capture.py.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/uart.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "huffman.h"
#if __has_include("build_mode.h")
#include "build_mode.h"        /* gerado por build.ps1: define DEMO no modo demonstracao */
#endif

/* ---------------- parametros (registrados no relatorio) ---------------- */
#ifndef HUFF_N
#define HUFF_N        1024u        /* tamanho do array: ~1,6 ms, mesma escala do Linux */
#endif
#define SEED          20261118u
#define APP_CORE      1
#define SYS_CORE      0
#define EVT_GPIO      GPIO_NUM_18
#ifdef DEMO                      /* modo demonstracao (build.ps1 -Demo): ~1 min de execucao */
#define ACTS1         1000
#define ACTS2         1000
#define ACTS34        1000
#else                            /* campanha do relatorio: ~6 min */
#define ACTS1         10000
#define ACTS2         3000
#define ACTS34        10000
#endif
#define CHUNK         (HUFF_N / 8)  /* sensor: mesma proporcao do Linux (4 KiB de 32 KiB) */
#define EVT_BYTES     (HUFF_N / 4)  /* pacote do evento: mesma proporcao (8 KiB de 32 KiB) */
#define SENSOR_US     250.0
#define LOGGER_US     1500.0
#define STACK_HUFF    12288      /* gen_codes usa ~7 KiB de pilha */

/* ---------------- relogio, PRNG, trabalho sintetico ---------------- */
/* Relogio da medicao, exigido pelo enunciado: clock_gettime(CLOCK_MONOTONIC).
 * No ESP-IDF ele e baseado no esp_timer (contador de hardware), com resolucao
 * de 1 us, nao depende do tick do FreeRTOS e nunca anda para tras. */
static inline uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* Gerador pseudoaleatorio xorshift64* com semente fixa (20261118): a mesma
 * semente gera sempre a mesma entrada, o que torna o experimento repetivel.
 * Usa aritmetica de 64 bits, que no Xtensa (32 bits) e emulada em software;
 * por isso a tarefa sensor custa ~1 ms, mais que os 250 us do laco calibrado. */
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
/* Caracteres ASCII imprimiveis (32..126). Usa o menor de dois sorteios para a
 * distribuicao nao ser uniforme: alguns simbolos ficam mais frequentes, como
 * num texto real, e a arvore de Huffman fica desbalanceada (~6,2 bits/simbolo). */
static void fill_random(uint8_t *buf, size_t n, uint64_t *s)
{
    for (size_t i = 0; i < n; i++) {
        uint64_t r = rng_next(s);
        uint32_t a = (uint32_t)(r % 95u), b = (uint32_t)((r >> 32) % 95u);
        buf[i] = (uint8_t)(32u + (a < b ? a : b));
    }
}

/* Trabalho sintetico de duracao conhecida (usado pelo sensor e pelo logger).
 * calib_task mede quantas iteracoes cabem em 1 us e converte SENSOR_US e
 * LOGGER_US em numero de iteracoes. g_sink impede o compilador de remover o laco. */
static volatile uint32_t g_sink;
static void busy_iters(uint32_t it)
{
    uint32_t x = 12345;
    for (uint32_t i = 0; i < it; i++) x = x * 1664525u + 1013904223u;
    g_sink = x;
}
static double g_iters_per_us;
static uint32_t g_sensor_iters, g_logger_iters;

/* ---------------- estado compartilhado ---------------- */
static uint8_t *g_shared, *g_local, *g_enc, *g_dec;
static uint8_t *g_pkt, *g_penc, *g_pdec;
static huff_ctx_t g_ctx, g_pctx;
static uint32_t *g_resp;
static uint16_t *g_lat, *g_wait;                         /* saturam em 65 535 us */
static volatile uint32_t g_fails, g_evt_count, g_dropped;
static volatile uint32_t g_uart_bytes, g_uart_ok, g_uart_bad;    /* estresse UART (etapa 4) */
static volatile int g_stop, g_abort;
static volatile uint32_t g_k;                            /* ativacoes concluidas */
static SemaphoreHandle_t g_done, g_mtx;
static TaskHandle_t g_main, g_gen_task, g_evt_task;
static volatile uint64_t g_t_emit;
static size_t g_wpos;
static uint64_t g_sensor_rs;

static void alloc_or_die(void **p, size_t sz, const char *what)
{
    *p = heap_caps_malloc(sz, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!*p) { printf("ERRO: sem memoria para %s (%u bytes)\n", what, (unsigned)sz); abort(); }
    memset(*p, 0, sz);
}

/* ================= despejo dos dados ================= */
/* Os tempos ficam em RAM durante a etapa e so sao enviados pela serial no fim:
 * imprimir durante a medicao acrescentaria interferencia (efeito da
 * instrumentacao). O formato ativacao,tempo_us e o pedido pelo enunciado. */
static void dump(const char *name, size_t n, int detail)
{
    printf("\n### INICIO %s n=%u falhas=%" PRIu32 " eventos=%" PRIu32 " uart_bytes=%" PRIu32
           " uart_quadros_ok=%" PRIu32 " uart_quadros_ruins=%" PRIu32 " liberacoes_perdidas=%" PRIu32 "\n",
           name, (unsigned)n, g_fails, g_evt_count, g_uart_bytes, g_uart_ok, g_uart_bad, g_dropped);
    printf(detail ? "ativacao,tempo_us,latencia_inicio_us,espera_mutex_us\n" : "ativacao,tempo_us\n");
    for (size_t i = 0; i < n; i++) {
        if (detail) printf("%u,%" PRIu32 ",%u,%u\n", (unsigned)(i + 1), g_resp[i], (unsigned)g_lat[i], (unsigned)g_wait[i]);
        else        printf("%u,%" PRIu32 "\n", (unsigned)(i + 1), g_resp[i]);
        if ((i & 63) == 63) vTaskDelay(1);   /* deixa a UART esvaziar */
    }
    printf("### FIM %s\n", name);
    fflush(stdout);
}

/* ================= resumo legivel de cada etapa ================= */
/* Mesmas definicoes de analysis/analyze.py: HWM(99%) = maximo depois de retirar
 * round(1% de n) maiores valores; perda se R > D; S = menor distancia entre perdas;
 * (m,20) = 20 - maior numero de perdas em uma janela deslizante de 20 ativacoes. */
static uint32_t g_D;                                      /* deadline fixo, definido na etapa 1 */

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void resumo(const char *name, size_t n)
{
    if (n == 0) return;
    uint32_t mn = UINT32_MAX, mx = 0; uint64_t sum = 0;
    uint32_t perdas = 0, S = 0, run = 0, maior = 0, win = 0, pior = 0;
    size_t last = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t r = g_resp[i];
        if (r < mn) mn = r;
        if (r > mx) mx = r;
        sum += r;
        int miss = r > g_D;
        if (miss) {
            if (perdas && (S == 0 || i + 1 - last < S)) S = (uint32_t)(i + 1 - last);
            last = i + 1; perdas++;
            if (++run > maior) maior = run;
        } else run = 0;
        /* janela deslizante k = 20: soma a perda que entra e tira a que sai
         * (ativacao i-20); a pior janela da m = 20 - pior */
        win += miss;
        if (i >= 20 && g_resp[i - 20] > g_D) win--;
        if (i >= 19 && win > pior) pior = win;
    }
    if (n < 20) pior = perdas;
    qsort(g_resp, n, sizeof g_resp[0], cmp_u32);         /* dados ja enviados; ordena para o HWM(99%) */
    size_t drop = (n + 50) / 100;
    uint32_t hwm99 = g_resp[n - drop - 1];
    char sbuf[16];
    if (S) snprintf(sbuf, sizeof sbuf, "%" PRIu32, S); else snprintf(sbuf, sizeof sbuf, "-");
    printf("\n### RESUMO %s: n=%u  min=%" PRIu32 "  media=%.1f  HWM=%" PRIu32 "  HWM99=%" PRIu32
           "  D=%" PRIu32 "  no_prazo=%.2f%%  perdas=%" PRIu32 "  S=%s  maior_rajada=%" PRIu32
           "  (m,20)=(%" PRIu32 ",20)  falhas_validacao=%" PRIu32 "\n",
           name, (unsigned)n, mn, (double)sum / n, mx, hwm99, g_D, 100.0 * (n - perdas) / n, perdas,
           sbuf, maior, 20 - pior, g_fails);
    fflush(stdout);
}

/* ================= Etapa 1 ================= */
/* Teste de tarefa: Huffman sozinho no nucleo 1, sem outras tarefas da
 * aplicacao. Tempo de resposta = fim - inicio da ativacao. O HWM desta etapa
 * define o deadline D usado em todas as etapas. */
static void etapa1_task(void *arg)
{
    uint64_t rs = rng_seed(SEED);
    for (size_t k = 0; k < ACTS1; k++) {
        fill_random(g_shared, HUFF_N, &rs);               /* vetor novo, gerado fora da medicao */
        uint64_t t0 = now_us();
        memcpy(g_local, g_shared, HUFF_N);
        int r = huff_task(&g_ctx, g_local, HUFF_N);
        uint64_t t1 = now_us();
        g_resp[k] = (uint32_t)(t1 - t0);
        g_fails += (r != 0);
    }
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

/* ================= GPIO: evento externo ================= */
/* Rotina de interrupcao da borda de subida no GPIO 18. Fica na IRAM (rapida,
 * independe do cache da flash) e so acorda a tarefa que trata o evento; o
 * trabalho pesado (Huffman) e feito fora da interrupcao. */
static void IRAM_ATTR gpio_isr(void *arg)
{
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(g_evt_task, &hp);
    portYIELD_FROM_ISR(hp);
}

static void gpio_setup_on_this_core(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ull << EVT_GPIO, .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_set_level(EVT_GPIO, 0);
    gpio_config(&io);
    gpio_install_isr_service(0);                         /* ISR alocada no nucleo chamador (1) */
    gpio_isr_handler_add(EVT_GPIO, gpio_isr, NULL);
}

static void gpio_teardown(void)
{
    gpio_isr_handler_remove(EVT_GPIO);
    gpio_uninstall_isr_service();
}

/* Gerador de eventos no nucleo 0: um evento por vez, intervalo aleatorio
 * [min,max] ms. O pino e configurado como entrada e saida ao mesmo tempo, entao
 * o proprio ESP32 gera a borda que provoca a interrupcao no nucleo 1 (sem fio
 * externo). g_t_emit guarda o instante da borda: e o inicio da medicao na etapa 2. */
typedef struct { int min_ms, max_ms; size_t count; } gen_cfg_t;
static void generator_task(void *arg)
{
    gen_cfg_t *cfg = (gen_cfg_t *)arg;
    uint64_t rs = rng_seed(SEED ^ 0xABCDEFull);
    for (size_t k = 0; !g_stop && (cfg->count == 0 || k < cfg->count); k++) {
        int gap = cfg->min_ms + (int)(rng_next(&rs) % (uint64_t)(cfg->max_ms - cfg->min_ms + 1));
        vTaskDelay(pdMS_TO_TICKS(gap));
        if (g_stop) break;
        g_t_emit = now_us();
        gpio_set_level(EVT_GPIO, 1);                     /* borda de subida -> interrupcao */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));    /* espera o ACK */
        gpio_set_level(EVT_GPIO, 0);
    }
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

/* ================= Etapa 2 ================= */
/* Teste comportamental: a tarefa espera o evento, roda o Huffman e confirma
 * (ACK) para o gerador liberar o proximo. Tempo de resposta = fim - instante da
 * borda, ou seja, inclui a interrupcao e o despacho da tarefa. */
static void etapa2_task(void *arg)
{
    gpio_setup_on_this_core();
    xSemaphoreGive(g_done);                              /* GPIO pronto */
    uint64_t rs = rng_seed(SEED);
    for (size_t k = 0; k < ACTS2; k++) {
        fill_random(g_shared, HUFF_N, &rs);              /* dado pronto antes do evento */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        memcpy(g_local, g_shared, HUFF_N);
        int r = huff_task(&g_ctx, g_local, HUFF_N);
        uint64_t t1 = now_us();
        g_resp[k] = (uint32_t)(t1 - g_t_emit);
        g_fails += (r != 0);
        xTaskNotifyGive(g_gen_task);                     /* ACK */
    }
    gpio_teardown();
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

/* ================= Etapas 3 e 4 ================= */
static QueueHandle_t q_sensor, q_huff, q_logger;
static volatile uint64_t g_base, g_tick;
static volatile int64_t g_isr_off;                       /* now_us() - esp_timer_get_time() */
#define REL_STOP UINT64_MAX

/* Interrupcao do timer de hardware (1 kHz) que libera as tarefas periodicas das
 * etapas 3 e 4. A cada tick, coloca na fila de cada tarefa o instante IDEAL da
 * liberacao (g_base + tick x 1 ms): sensor a cada 4 ticks, huffman a cada 10,
 * logger a cada 33. A medicao parte desse instante ideal, entao qualquer atraso
 * (espera, preempcao, bloqueio, a propria interrupcao atrasada) entra no tempo de
 * resposta. Se a interrupcao for adiada, o laco recupera os ticks perdidos.
 * No tick 1 as tres tarefas sao liberadas juntas (instante critico). */
static bool IRAM_ATTR on_tick(gptimer_handle_t t, const gptimer_alarm_event_data_t *e, void *ctx)
{
    BaseType_t hp = pdFALSE;
    int64_t now = esp_timer_get_time() + g_isr_off;
    uint64_t target = (uint64_t)(now - (int64_t)g_base) / 1000ull;
    while (g_tick <= target) {                           /* recupera ticks atrasados (ISR adiada) */
        uint64_t rel = g_base + g_tick * 1000ull;        /* instante ideal de liberacao */
        uint64_t j = g_tick - 1;                         /* tick 1 = instante critico */
        if (j % 4 == 0  && xQueueSendFromISR(q_sensor, &rel, &hp) != pdTRUE) g_dropped++;
        if (j % 10 == 0 && xQueueSendFromISR(q_huff,   &rel, &hp) != pdTRUE) g_dropped++;
        if (j % 33 == 0 && xQueueSendFromISR(q_logger, &rel, &hp) != pdTRUE) g_dropped++;
        g_tick++;
    }
    return hp == pdTRUE;
}

/* sensor: T = 4 ms, prioridade 12 (a maior: menor periodo, rate monotonic).
 * Processa (~250 us de laco) e escreve 128 B no buffer compartilhado sob o mutex.
 * Como e liberado a cada 4 ms e o Huffman dura ~2,2 ms, ele interrompe toda
 * ativacao do huffman uma vez -> e a causa dos 0% no prazo na etapa 3. */
static void sensor_task(void *arg)
{
    uint64_t rel;
    for (;;) {
        xQueueReceive(q_sensor, &rel, portMAX_DELAY);
        if (rel == REL_STOP) break;
        busy_iters(g_sensor_iters);
        xSemaphoreTake(g_mtx, portMAX_DELAY);
        fill_random(g_shared + g_wpos, CHUNK, &g_sensor_rs);
        g_wpos = (g_wpos + CHUNK) % HUFF_N;
        xSemaphoreGive(g_mtx);
    }
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

/* logger: T = 33 ms, prioridade 8 (a menor). Le o buffer inteiro e segura o mutex
 * por ~1,5 ms. Quando o huffman chega nesse intervalo, fica bloqueado esperando o
 * mutex (bloqueio B da analise de tempo de resposta). Pela heranca de prioridade
 * do mutex do FreeRTOS, o logger roda com a prioridade do huffman enquanto o
 * segura, e nenhuma tarefa intermediaria passa na frente. */
static void logger_task(void *arg)
{
    uint64_t rel;
    for (;;) {
        xQueueReceive(q_logger, &rel, portMAX_DELAY);
        if (rel == REL_STOP) break;
        xSemaphoreTake(g_mtx, portMAX_DELAY);
        uint32_t sum = 0;
        for (size_t i = 0; i < HUFF_N; i++) sum = sum * 31u + g_shared[i];
        g_sink ^= sum;
        busy_iters(g_logger_iters);
        xSemaphoreGive(g_mtx);
    }
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

/* huffman: T = 10 ms, prioridade 10. E a tarefa sob teste nas etapas 3 e 4.
 * Copia o buffer compartilhado sob o mutex e roda o Huffman fora dele.
 * Ativacoes atrasadas nao sao descartadas: as liberacoes esperam na fila. */
static void huffman_periodic_task(void *arg)
{
    uint64_t rel;
    g_k = 0;
    for (size_t k = 0; k < ACTS34 && !g_abort; k++) {
        if (xQueueReceive(q_huff, &rel, pdMS_TO_TICKS(200)) != pdTRUE) { k--; continue; }
        uint64_t ts = now_us();                           /* inicio real da execucao */
        xSemaphoreTake(g_mtx, portMAX_DELAY);             /* pode esperar o logger (bloqueio) */
        uint64_t tl = now_us();                           /* conseguiu o mutex */
        memcpy(g_local, g_shared, HUFF_N);
        xSemaphoreGive(g_mtx);
        int r = huff_task(&g_ctx, g_local, HUFF_N);
        uint64_t te = now_us();                           /* fim da ativacao */
        g_resp[k] = (uint32_t)(te - rel);                 /* tempo de resposta = fim - liberacao ideal */
        /* detalhe usado na analise das causas: latencia de inicio e espera no mutex */
        uint64_t lat = ts > rel ? ts - rel : 0, wt = tl - ts;
        g_lat[k]  = (uint16_t)(lat > 65535 ? 65535 : lat);
        g_wait[k] = (uint16_t)(wt > 65535 ? 65535 : wt);
        g_fails += (r != 0);
        g_k = (uint32_t)(k + 1);
    }
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

/* Tarefa esporadica da etapa 4 (prioridade 11, acima do huffman): trata o evento
 * GPIO codificando um pacote de 256 B. Os eventos chegam a cada 2-8 ms. */
static void event_task(void *arg)
{
    gpio_setup_on_this_core();
    xSemaphoreGive(g_done);
    uint64_t rs = rng_seed(SEED ^ 0x777ull);
    while (!g_stop) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50)) == 0) continue;
        fill_random(g_pkt, EVT_BYTES, &rs);
        g_fails += (huff_task(&g_pctx, g_pkt, EVT_BYTES) != 0);
        g_evt_count++;
        xTaskNotifyGive(g_gen_task);
    }
    gpio_teardown();
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

/* ---------------- carga externa (etapa 4): rajada UART vinda do PC ----------------
 * O PC (esp32/capture.py) envia, durante toda a etapa 4, quadros deterministicos
 * (semente fixa) pela mesma UART0 do console, na taxa maxima da linha (115200 baud):
 *     A5 5A | seq (2 B) | payload (256 B) | soma (2 B)   = 262 B por quadro
 * O driver UART e instalado a partir do NUCLEO 1 (ISR no nucleo das tarefas sob teste)
 * com limiar de FIFO de 1 byte: pior caso de uma interrupcao por byte recebido
 * (~11 500 interrupcoes/s). Cada quadro valido e tratado por uma tarefa de prioridade
 * 11 (acima do huffman), que valida a soma e codifica o payload com Huffman. */
#define UART_FRAME   262
#define UART_PAYLOAD 256

static uint8_t *g_upay, *g_uenc, *g_udec;
static huff_ctx_t g_uctx;

static void uart_rx_task(void *arg)
{
    uart_config_t uc = {
        .baud_rate = 115200, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    uart_wait_tx_idle_polling(UART_NUM_0);                   /* termina de enviar o texto pendente */
    uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0);    /* ISR alocada no nucleo 1 */
    uart_param_config(UART_NUM_0, &uc);
    uart_set_rx_full_threshold(UART_NUM_0, 1);               /* uma interrupcao por byte */
    uart_set_rx_timeout(UART_NUM_0, 1);
    uart_flush_input(UART_NUM_0);
    xSemaphoreGive(g_done);

    static uint8_t chunk[64];
    static uint8_t frame[UART_FRAME];
    int pos = 0;
    while (!g_stop) {
        int n = uart_read_bytes(UART_NUM_0, chunk, sizeof chunk, pdMS_TO_TICKS(20));
        /* maquina de estados simples: procura o cabecalho A5 5A, junta 262 bytes,
         * confere a soma do payload e, se estiver certa, codifica o payload */
        for (int i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            g_uart_bytes++;
            if (pos == 0 && b != 0xA5) continue;               /* sincroniza no cabecalho */
            if (pos == 1 && b != 0x5A) { pos = (b == 0xA5); continue; }
            frame[pos++] = b;
            if (pos == UART_FRAME) {
                uint16_t sum = 0;
                for (int j = 4; j < 4 + UART_PAYLOAD; j++) sum = (uint16_t)(sum + frame[j]);
                uint16_t rx = (uint16_t)(frame[260] | (frame[261] << 8));
                if (sum == rx) {
                    memcpy(g_upay, frame + 4, UART_PAYLOAD);
                    g_fails += (huff_task(&g_uctx, g_upay, UART_PAYLOAD) != 0);
                    g_uart_ok++;
                } else {
                    g_uart_bad++;
                }
                pos = 0;
            }
        }
    }
    uart_driver_delete(UART_NUM_0);
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

static void spawn(TaskFunction_t f, const char *nm, uint32_t stk, void *arg, UBaseType_t prio, TaskHandle_t *h, int core)
{
    if (xTaskCreatePinnedToCore(f, nm, stk, arg, prio, h, core) != pdPASS) {
        printf("ERRO: nao criou tarefa %s (pilha %u, heap livre %u, maior bloco %u)\n", nm, (unsigned)stk,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        abort();
    }
}

static void wait_done(int n)
{
    for (int i = 0; i < n; i++) xSemaphoreTake(g_done, portMAX_DELAY);
}

static void timer_ctl_task(void *arg)
{
    gptimer_handle_t tm = NULL;
    gptimer_config_t tc = { .clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP, .resolution_hz = 1000000 };
    ESP_ERROR_CHECK(gptimer_new_timer(&tc, &tm));
    gptimer_event_callbacks_t cbs = { .on_alarm = on_tick };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(tm, &cbs, NULL));
    gptimer_alarm_config_t ac = { .alarm_count = 1000, .reload_count = 0, .flags.auto_reload_on_alarm = true };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(tm, &ac));
    ESP_ERROR_CHECK(gptimer_enable(tm));                 /* aloca a interrupcao neste nucleo */
    g_isr_off = (int64_t)now_us() - esp_timer_get_time();
    g_tick = 1;
    g_base = now_us();
    ESP_ERROR_CHECK(gptimer_start(tm));
    xSemaphoreGive(g_done);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);             /* espera o fim da etapa */
    gptimer_stop(tm); gptimer_disable(tm); gptimer_del_timer(tm);
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

static uint32_t run_periodic(int stage)
{
    fflush(stdout);
    g_stop = 0; g_abort = 0; g_fails = 0; g_evt_count = 0; g_dropped = 0;
    g_uart_bytes = 0; g_uart_ok = 0; g_uart_bad = 0;
    q_sensor = xQueueCreate(64, sizeof(uint64_t));
    q_huff   = xQueueCreate(256, sizeof(uint64_t));
    q_logger = xQueueCreate(64, sizeof(uint64_t));
    g_sensor_rs = rng_seed(SEED);
    fill_random(g_shared, HUFF_N, &g_sensor_rs);
    g_wpos = 0;

    /* Etapa 4 = etapa 3 + carga externa: eventos GPIO e trafego UART do PC.
     * As cargas sao iniciadas antes do timer e estabilizam por 1,5 s. */
    int extra = 0;
    static gen_cfg_t gen4 = { 2, 8, 0 };
    if (stage == 4) {
        spawn(event_task, "evento", STACK_HUFF, NULL, 11, &g_evt_task, APP_CORE);
        wait_done(1);                                     /* GPIO configurado no nucleo 1 */
        spawn(generator_task, "gerador", 4096, &gen4, 5, &g_gen_task, SYS_CORE);
        spawn(uart_rx_task, "uart_rx", STACK_HUFF, NULL, 11, NULL, APP_CORE);
        wait_done(1);                                     /* driver UART instalado no nucleo 1 */
        extra = 3;                                        /* evento, gerador, uart_rx */
        printf("\n### UART_ESTRESSE ON\n");               /* o PC comeca a enviar */
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1500));                  /* carga estabiliza */
    }

    spawn(sensor_task, "sensor", 4096, NULL, 12, NULL, APP_CORE);
    spawn(logger_task, "logger", 4096, NULL, 8, NULL, APP_CORE);
    spawn(huffman_periodic_task, "huffman", STACK_HUFF, NULL, 10, NULL, APP_CORE);

    /* timer de hardware de 1 kHz, configurado a partir do nucleo 1 (ISR no nucleo das tarefas) */
    TaskHandle_t tt;
    spawn(timer_ctl_task, "timer", 4096, NULL, 20, &tt, APP_CORE);
    wait_done(1);                                         /* timer iniciado */
    printf("etapa%d: timer iniciado\n", stage);

    /* espera o huffman terminar; progresso a cada 20 s; limite de 180 s por etapa */
    uint64_t t_ini = now_us();
    while (xSemaphoreTake(g_done, pdMS_TO_TICKS(20000)) != pdTRUE) {
        printf("progresso etapa%d: %" PRIu32 "/%d ativacoes, liberacoes perdidas %" PRIu32
               ", eventos %" PRIu32 ", uart bytes %" PRIu32 ", quadros ok %" PRIu32 ", ruins %" PRIu32 "\n",
               stage, g_k, ACTS34, g_dropped, g_evt_count, g_uart_bytes, g_uart_ok, g_uart_bad);
        if (now_us() - t_ini > 180000000ull) g_abort = 1;
    }
    g_stop = 1;
    if (stage == 4) { printf("\n### UART_ESTRESSE OFF\n"); fflush(stdout); }   /* o PC para de enviar */
    xTaskNotifyGive(tt);
    wait_done(1);                                         /* timer parado */
    uint64_t stop = REL_STOP;
    xQueueSend(q_sensor, &stop, portMAX_DELAY);
    xQueueSend(q_logger, &stop, portMAX_DELAY);
    wait_done(2 + extra);
    if (stage == 4) vTaskDelay(pdMS_TO_TICKS(1500));     /* linha serial esvazia antes do despejo */
    vQueueDelete(q_sensor); vQueueDelete(q_huff); vQueueDelete(q_logger);
    return g_k;
}

/* ================= Experimento A: casos de teste no ESP32 ================= */
/* Mesmos 7 casos de tests/test_huffman.c, com N = 1024 (cobertura medida no host com
 * gcov para os mesmos tamanhos: scripts/coverage.sh, NMAX=1024). */
static void gen_random_c(uint8_t *b, size_t n) { uint64_t s = rng_seed(SEED); fill_random(b, n, &s); }
static void gen_one_c(uint8_t *b, size_t n)    { memset(b, 'A', n); }
static void gen_two_c(uint8_t *b, size_t n)    { for (size_t i = 0; i < n; i++) b[i] = (i % 3) ? 'x' : 'y'; }
static void gen_all_c(uint8_t *b, size_t n)    { for (size_t i = 0; i < n; i++) b[i] = (uint8_t)i; }
static void gen_empty_c(uint8_t *b, size_t n)  { (void)b; (void)n; }
static void gen_fib_c(uint8_t *b, size_t n)
{
    size_t f[20] = { 1, 1 }, pos = 0;
    for (int i = 2; i < 20; i++) f[i] = f[i - 1] + f[i - 2];
    for (int s = 0; s < 20 && pos < n; s++)
        for (size_t k = 0; k < f[s] && pos < n; k++) b[pos++] = (uint8_t)('a' + s);
    while (pos < n) b[pos++] = 'a';
}
typedef struct { const char *desc; size_t n; void (*gen)(uint8_t *, size_t); int expect; } tcase_t;
static const tcase_t k_cases[] = {
    { "aleatorio N=1024 (mesma entrada da etapa 1)",  HUFF_N, gen_random_c, 0 },
    { "entrada vazia (n=0)",                          0,      gen_empty_c,  0 },
    { "um unico simbolo repetido (n=1000)",           1000,   gen_one_c,    0 },
    { "dois simbolos (n=999)",                        999,    gen_two_c,    0 },
    { "256 simbolos com frequencias iguais",          256,    gen_all_c,    0 },
    { "frequencias de Fibonacci (n=1024)",            HUFF_N, gen_fib_c,    0 },
    { "validacao detecta saida corrompida",           64,     gen_random_c, -1 },
};

static void cases_task(void *arg)
{
    const int reps = 200;
    printf("\n### INICIO casos_teste n=%u\ncaso,descricao,n,repeticoes,ok,hwm_us\n", (unsigned)(sizeof k_cases / sizeof k_cases[0]));
    for (size_t c = 0; c < sizeof k_cases / sizeof k_cases[0]; c++) {
        const tcase_t *t = &k_cases[c];
        uint32_t hwm = 0; int got = 0;
        for (int r = 0; r < reps; r++) {
            t->gen(g_shared, t->n);                      /* fora da medicao */
            uint64_t t0 = now_us();
            if (t->expect == 0) {
                got = huff_task(&g_ctx, g_shared, t->n);
            } else {
                huff_encode(&g_ctx, g_shared, t->n);
                huff_decode(&g_ctx, t->n);
                g_ctx.dec[t->n / 2] ^= 0x01;
                got = huff_validate(g_shared, g_ctx.dec, t->n);
            }
            uint32_t dt = (uint32_t)(now_us() - t0);
            if (dt > hwm) hwm = dt;
        }
        printf("%u,%s,%u,%d,%s,%" PRIu32 "\n", (unsigned)(c + 1), t->desc, (unsigned)t->n, reps,
               got == t->expect ? "sim" : "NAO", hwm);
    }
    printf("### FIM casos_teste\n");
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

/* ================= calibracao e main ================= */
static void calib_task(void *arg)
{
    /* trabalho sintetico: iteracoes por us, sem carga */
    uint32_t best = UINT32_MAX;
    for (int k = 0; k < 5; k++) {
        uint64_t t0 = now_us(); busy_iters(2000000); uint32_t dt = (uint32_t)(now_us() - t0);
        if (dt < best) best = dt;
    }
    g_iters_per_us = 2000000.0 / best;
    g_sensor_iters = (uint32_t)(SENSOR_US * g_iters_per_us);
    g_logger_iters = (uint32_t)(LOGGER_US * g_iters_per_us);
    /* tempo nominal do Huffman para alguns N (escolha do N) */
    static const size_t ns[] = { 512, 768, 1024, 1536, 2048 };
    uint64_t rs = rng_seed(SEED);
    for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
        uint32_t mn = UINT32_MAX; uint64_t sum = 0;
        for (int r = 0; r < 50; r++) {
            fill_random(g_shared, ns[i], &rs);
            uint64_t t0 = now_us(); huff_task(&g_ctx, g_shared, ns[i]); uint32_t dt = (uint32_t)(now_us() - t0);
            if (dt < mn) mn = dt;
            sum += dt;
        }
        printf("calib N=%u: min %" PRIu32 " us, media %.1f us\n", (unsigned)ns[i], mn, sum / 50.0);
    }
    xSemaphoreGive(g_done);
    vTaskDelete(NULL);
}

void app_main(void)
{
    g_main = xTaskGetCurrentTaskHandle();
    vTaskPrioritySet(NULL, 7);                           /* orquestrador: quase sempre bloqueado */
    g_done = xSemaphoreCreateCounting(16, 0);
    g_mtx = xSemaphoreCreateMutex();                     /* mutex FreeRTOS: heranca de prioridade */

    alloc_or_die((void **)&g_shared, 2048, "shared");
    alloc_or_die((void **)&g_local, 2048, "local");
    alloc_or_die((void **)&g_enc, 3 * 2048 + 8, "enc");  /* codigos <= 17 bits p/ n <= 4096 */
    alloc_or_die((void **)&g_dec, 2048, "dec");
    alloc_or_die((void **)&g_pkt, EVT_BYTES, "pkt");
    alloc_or_die((void **)&g_penc, 3 * EVT_BYTES + 8, "penc");
    alloc_or_die((void **)&g_pdec, EVT_BYTES, "pdec");
    huff_setup(&g_ctx, g_enc, g_dec);
    huff_setup(&g_pctx, g_penc, g_pdec);
    alloc_or_die((void **)&g_upay, UART_PAYLOAD, "upay");
    alloc_or_die((void **)&g_uenc, 3 * UART_PAYLOAD + 8, "uenc");
    alloc_or_die((void **)&g_udec, UART_PAYLOAD, "udec");
    huff_setup(&g_uctx, g_uenc, g_udec);
    alloc_or_die((void **)&g_resp, ACTS34 * sizeof(uint32_t), "resp");
    alloc_or_die((void **)&g_lat, ACTS34 * sizeof(uint16_t), "lat");
    alloc_or_die((void **)&g_wait, ACTS34 * sizeof(uint16_t), "wait");

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_chip_info_t ci; esp_chip_info(&ci);
    uint32_t fsz = 0; esp_flash_get_size(NULL, &fsz);
    printf("\n### PLATAFORMA\n");
    printf("chip: ESP32 modelo %d rev %d.%d, %d nucleos, flash %" PRIu32 " MB\n", ci.model, ci.revision / 100, ci.revision % 100, ci.cores, fsz >> 20);
    printf("ESP-IDF: %s, FreeRTOS tick %d Hz, CPU 240 MHz fixa (sem DFS)\n", esp_get_idf_version(), configTICK_RATE_HZ);
    printf("N=%u semente=%u CHUNK=%u EVT_BYTES=%u heap livre=%u\n", (unsigned)HUFF_N, (unsigned)SEED,
           (unsigned)CHUNK, (unsigned)EVT_BYTES, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    spawn(calib_task, "calib", STACK_HUFF, NULL, 10, NULL, APP_CORE);
    wait_done(1);
    printf("busy: %.2f iter/us -> sensor %" PRIu32 " iter, logger %" PRIu32 " iter\n", g_iters_per_us, g_sensor_iters, g_logger_iters);
    printf("ativacoes por etapa: %d / %d / %d / %d%s\n", ACTS1, ACTS2, ACTS34, ACTS34,
#ifdef DEMO
           "  (MODO DEMONSTRACAO)"
#else
           "  (campanha completa)"
#endif
           );
    printf("### FIM PLATAFORMA\n");

    /* Experimento A: casos de teste (tempo de cada caso no ESP32) */
    spawn(cases_task, "casos", STACK_HUFF, NULL, 10, NULL, APP_CORE);
    wait_done(1);

#ifdef SO_ETAPA4                                          /* teste rapido da etapa 4 */
    { uint32_t n4 = run_periodic(4); dump("etapa4_sistema", n4, 1); printf("\n### CAMPANHA CONCLUIDA\n"); for (;;) vTaskDelay(1000); }
#endif
    /* Etapa 1 */
    g_fails = 0;
    spawn(etapa1_task, "etapa1", STACK_HUFF, NULL, 10, NULL, APP_CORE);
    wait_done(1);
    printf("etapa1: bits codificados %u (%.2f bits/simbolo)\n", (unsigned)g_ctx.enc_bits, (double)g_ctx.enc_bits / HUFF_N);
    dump("etapa1_tarefa", ACTS1, 0);
    {   /* D fixo = 1,2 x HWM(100%) da etapa 1, o mesmo para as quatro etapas
         * (regra sugerida no enunciado; mudar D entre etapas impediria comparar) */
        uint32_t h1 = 0;
        for (size_t i = 0; i < ACTS1; i++) if (g_resp[i] > h1) h1 = g_resp[i];
        g_D = (uint32_t)(1.2 * h1);
    }
    resumo("etapa1_tarefa", ACTS1);

    /* Etapa 2 */
    g_fails = 0; g_stop = 0;
    static gen_cfg_t gen2 = { 2, 6, ACTS2 };
    spawn(etapa2_task, "etapa2", STACK_HUFF, NULL, 10, &g_evt_task, APP_CORE);
    wait_done(1);
    spawn(generator_task, "gerador", 4096, &gen2, 5, &g_gen_task, SYS_CORE);
    wait_done(2);
    dump("etapa2_comportamental", ACTS2, 0);
    resumo("etapa2_comportamental", ACTS2);

    /* Etapas 3 e 4 */
    uint32_t n3 = run_periodic(3);
    dump("etapa3_intertarefas", n3, 1);
    resumo("etapa3_intertarefas", n3);
    uint32_t n4 = run_periodic(4);
    dump("etapa4_sistema", n4, 1);
    resumo("etapa4_sistema", n4);

    printf("\n### CAMPANHA CONCLUIDA\n");
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}
