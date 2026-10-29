/* M2: adaptacao dos exemplos WiFi/socket e SNTP de Felipe Viel.
 * As tasks de rede/hora ficam abaixo das prioridades da M1, no mesmo core.
 * Sockets nao bloqueantes: um PC por ensaio, protocolo escolhido no menuconfig.
 */
#include "m2.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

#if CONFIG_M2_TCP
#define PROTO "TCP"
#else
#define PROTO "UDP"
#endif

typedef struct {
    const char *task, *origem;
    int64_t evt, start, end;
    uint32_t deadline;
} metric_t;
static QueueHandle_t qLog;
static volatile uint32_t log_drops, input_drops;
static volatile bool wifi_ready;
static portMUX_TYPE time_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t last_sync_us;
static uint32_t sync_count;
static uint32_t cycles_since_sync;
static int fd = -1, peer_fd = -1;
static struct sockaddr_in peer;
static socklen_t peer_len = sizeof(peer);
static bool peer_known;
static volatile bool measurement_started;
static uint32_t packet_seq, send_errors, disconnects;
static char pending[768];
static size_t pending_len, pending_offset;

static void begin_session(void)
{
    // Sem logs de drivers/WiFi na UART durante a janela de medicao.
    esp_log_level_set("*", ESP_LOG_NONE);
    measurement_started = true;
    // A coleta comeca quando o PC chega; descarta somente o aquecimento.
    // Depois disso nao ha reset de perdas durante o ensaio.
    xQueueReset(qLog);
    log_drops = 0;
    input_drops = 0;
    packet_seq = 0;
}

void m2_init(void)
{
    qLog = xQueueCreate(512, sizeof(metric_t));
    configASSERT(qLog);
}

void m2_input_drop(void) { input_drops++; } // chamadas em ISR ou core 0

void m2_log(const char *task, int64_t evt, int64_t start, int64_t end,
            uint32_t deadline, const char *origem)
{
    metric_t m = { task, origem, evt, start, end, deadline };
    // evt/start/end ja foram capturados em variaveis antes desta chamada.
    // Apenas copia binaria em RAM: sem printf, formatacao ou socket aqui.
    // Strings devem ser literais estaticas.
    if (xQueueSend(qLog, &m, 0) != pdTRUE) log_drops++;
}

static int64_t epoch_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void sync_cb(struct timeval *tv)
{
    portENTER_CRITICAL(&time_lock);
    last_sync_us = esp_timer_get_time();
    sync_count++;
    cycles_since_sync = 0;
    portEXIT_CRITICAL(&time_lock);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_ready = false;
        esp_wifi_connect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        wifi_ready = true;
        ip_event_got_ip_t *ip = data;
        // Aviso de configuracao apenas no aquecimento, nunca na coleta.
        if (!measurement_started) {
            printf("M2 IP=" IPSTR " protocolo=%s porta=%d CPU=%d MHz\n",
                   IP2STR(&ip->ip_info.ip), PROTO, CONFIG_M2_PORT,
                   CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
        }
    }
}

static void wifi_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", CONFIG_M2_WIFI_SSID);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", CONFIG_M2_WIFI_PASS);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// Retem o restante de uma escrita TCP parcial: TCP nao preserva mensagens.
static bool flush_pending(void)
{
    if (!pending_len) return true;
    int n;
#if CONFIG_M2_TCP
    n = send(peer_fd, pending + pending_offset, pending_len - pending_offset, 0);
#else
    n = sendto(fd, pending, pending_len, 0, (struct sockaddr *)&peer, peer_len);
#endif
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        send_errors++;
#if CONFIG_M2_TCP
        close(peer_fd);
        peer_fd = -1;
        disconnects++;
#endif
        pending_len = pending_offset = 0;
        return false;
    }
    pending_offset += n;
    if (pending_offset == pending_len) pending_len = pending_offset = 0;
    return pending_len == 0;
}

static void packet(const char *body)
{
    uint32_t sync;
    portENTER_CRITICAL(&time_lock);
    sync = sync_count;
    portEXIT_CRITICAL(&time_lock);
    // bytes eh string de largura fixa para preencher sem mudar o tamanho.
    int n = snprintf(pending, sizeof(pending),
        "{\"seq\":%lu,\"proto\":\"%s\",\"bytes\":\"0000\",\"esp_ms\":%lld,"
        "\"tx_us\":%lld,\"synced\":%s,%s}\n",
        (unsigned long)packet_seq++, PROTO, (long long)epoch_ms(),
        (long long)esp_timer_get_time(), sync ? "true" : "false", body);
    configASSERT(n > 0 && n < sizeof(pending));
    char size[5];
    snprintf(size, sizeof(size), "%04u", (unsigned)n);
    memcpy(strstr(pending, "\"bytes\":\"") + 9, size, 4);
    pending_len = n;
    pending_offset = 0;
    flush_pending();
}

static void task_time(void *arg)
{
    bool started = false;
    TickType_t next = xTaskGetTickCount();
    int64_t evt = esp_timer_get_time();
    for (;;) {
        int64_t start = esp_timer_get_time();
        if (!started && wifi_ready) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_set_time_sync_notification_cb(sync_cb);
            esp_sntp_setservername(0, CONFIG_M2_SNTP_SERVER);
            esp_sntp_set_sync_interval(CONFIG_M2_SNTP_INTERVAL_MS);
            esp_sntp_init();
            started = true;
        }
        // SNTP e assincrono. Sem sync: contador cresce, hora nao e valida.
        struct timeval tv;
        gettimeofday(&tv, NULL);
        portENTER_CRITICAL(&time_lock);
        cycles_since_sync++;
        portEXIT_CRITICAL(&time_lock);
        int64_t end = esp_timer_get_time();
        m2_log("TIME", evt, start, end, 1000000, "periodic");
        evt += 1000000;
        vTaskDelayUntil(&next, pdMS_TO_TICKS(1000));
    }
}

static bool open_socket(void)
{
#if CONFIG_M2_TCP
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
#else
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
#endif
    if (fd < 0) return false;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    fcntl(fd, F_SETFL, O_NONBLOCK);
    struct sockaddr_in addr = {.sin_family = AF_INET,
        .sin_port = htons(CONFIG_M2_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto fail;
#if CONFIG_M2_TCP
    if (listen(fd, 1) < 0) goto fail;
#endif
    return true;
fail:
    close(fd); fd = -1; return false;
}

static void task_monitor(void *arg)
{
    wifi_init();
    TickType_t next = xTaskGetTickCount();
    int64_t evt = esp_timer_get_time();
    char rx[128] = {0};
    size_t used = 0;
    uint32_t previous_idle = 0, previous_total = 0;
    bool cpu_first = true;
    for (;;) {
        int64_t start = esp_timer_get_time();
        if (wifi_ready && fd < 0) open_socket();
#if CONFIG_M2_TCP
        if (fd >= 0 && peer_fd < 0) {
            peer_fd = accept(fd, NULL, NULL);
            if (peer_fd >= 0) {
                fcntl(peer_fd, F_SETFL, O_NONBLOCK);
                used = 0;
                begin_session();
                cpu_first = true;
            }
        }
        bool connected = peer_fd >= 0;
        int sock = peer_fd;
#else
        bool connected = fd >= 0 && peer_known;
        int sock = fd;
#endif
        // No maximo um comando por ciclo, para limitar carga na task soft.
        if (sock >= 0 && !pending_len) {
#if CONFIG_M2_TCP
            // Ler ate uma linha sem descartar comandos TCP concatenados.
            int n = -1;
            while (used < sizeof(rx)-1) {
                n = recv(sock, rx + used, 1, 0);
                if (n <= 0) break;
                if (rx[used++] == '\n') break;
            }
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(peer_fd); peer_fd = -1; connected = false; used = 0; disconnects++;
            }
#else
            int n = recvfrom(sock, rx, sizeof(rx)-1, 0, (struct sockaddr *)&peer, &peer_len);
            if (n > 0) {
                if (!peer_known) { begin_session(); cpu_first = true; }
                used = n;
                peer_known = connected = true;
            }
#endif
            if (used && (rx[used-1] == '\n' || used == sizeof(rx)-1)) {
                rx[used] = 0;
                unsigned long seq;
                long long pc_ms;
                char cmd[16], extra;
                if (sscanf(rx, "%lu,%lld,%15s %c", &seq, &pc_ms, cmd, &extra) == 3 &&
                    (!strcmp(cmd,"SORT") || !strcmp(cmd,"ESTOP") || !strcmp(cmd,"PING"))) {
                    int64_t received = esp_timer_get_time();
                    m2_command(cmd, received);
                    char body[256];
                    snprintf(body, sizeof(body), "\"type\":\"ack\",\"cmd_seq\":%lu,"
                        "\"pc_ms\":%lld,\"cmd\":\"%s\",\"rx_us\":%lld",
                        seq, pc_ms, cmd, (long long)received);
                    packet(body);
                }
                used = 0;
            }
        }
        if (connected && flush_pending()) {
            // Orçamento de 100 ms de parede; falhas/pressao aparecem nos contadores.
            metric_t m;
            while (esp_timer_get_time() - start < 100000 &&
                   xQueueReceive(qLog, &m, 0) == pdTRUE) {
                char body[384];
                snprintf(body, sizeof(body), "\"type\":\"job\",\"task\":\"%s\","
                    "\"origin\":\"%s\",\"evt\":%lld,\"start\":%lld,\"end\":%lld,\"d\":%lu",
                    m.task, m.origem, (long long)m.evt, (long long)m.start,
                    (long long)m.end, (unsigned long)m.deadline);
                packet(body);
                if (pending_len) break;
            }
            if (!pending_len) {
                // CPU real via tempo de idle; primeiro intervalo e descartado.
                TaskStatus_t tasks[32];
                configRUN_TIME_COUNTER_TYPE total = 0;
                UBaseType_t count = uxTaskGetSystemState(tasks, 32, &total);
                uint32_t idle = 0;
                for (UBaseType_t i=0; i<count; i++)
                    if (!strncmp(tasks[i].pcTaskName, "IDLE", 4)) idle += tasks[i].ulRunTimeCounter;
                uint32_t dt = (uint32_t)total - previous_total;
                uint32_t di = idle - previous_idle;
                float cpu = (!cpu_first && count && dt && di <= dt) ? 100.f*(1.f-(float)di/dt) : -1.f;
                cpu_first = false; previous_idle = idle; previous_total = total;
                int64_t sync_us;
                uint32_t sync, cycles;
                portENTER_CRITICAL(&time_lock);
                sync_us = last_sync_us; sync = sync_count; cycles = cycles_since_sync;
                portEXIT_CRITICAL(&time_lock);
                char body[512];
                snprintf(body, sizeof(body), "\"type\":\"status\",\"cpu_pct\":%.2f,"
                    "\"log_drops\":%lu,\"input_drops\":%lu,\"send_errors\":%lu,"
                    "\"disconnects\":%lu,\"sync_count\":%lu,\"cycles_since_sync\":%lu,"
                    "\"sync_age_ms\":%lld,\"freq_mhz\":%d",
                    cpu, (unsigned long)log_drops, (unsigned long)input_drops,
                    (unsigned long)send_errors, (unsigned long)disconnects,
                    (unsigned long)sync, (unsigned long)cycles,
                    sync ? (long long)((esp_timer_get_time()-sync_us)/1000) : -1LL,
                    CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
                packet(body);
            }
        }
        // Rede/formatacao sao trabalho real da MONITOR e integram seu tempo.
        // A copia deste registro para RAM ocorre somente depois de capturar end.
        int64_t end = esp_timer_get_time();
        m2_log("MONITOR", evt, start, end, 500000, "periodic");
        evt += 500000;
        vTaskDelayUntil(&next, pdMS_TO_TICKS(500));
    }
}

void m2_start(void)
{
    BaseType_t time_ok = xTaskCreatePinnedToCore(task_time, "TIME", 4096, NULL, 1, NULL, 0);
    BaseType_t monitor_ok = xTaskCreatePinnedToCore(task_monitor, "MONITOR", 8192, NULL, 1, NULL, 0);
    configASSERT(time_ok == pdPASS && monitor_ok == pdPASS);
    (void)time_ok;
    (void)monitor_ok;
}
