/* M2: WiFi/socket e SNTP adaptados dos exemplos de Felipe Viel.
 * Metricas online em RAM. A rede envia resumos, nao cada job do encoder.
 * Sem prints durante a janela de medicao. ESP-IDF instalado: 6.1.
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
#include "esp_event.h"
#include "esp_log.h"
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
#define N_TASKS 6
#define N_SAMPLES 8
#define FIRM_K 10
#define FIRM_M 9
static const char *names[N_TASKS] = {"ENC", "CTRL", "SORT", "SAFETY", "MONITOR", "TIME"};
static const uint32_t periods[N_TASKS] = {5000, 5000, 0, 0, 500000, 1000000};
static int64_t first_release_us[N_TASKS];

typedef struct {
    int64_t evt, start, end;
    const char *origin;
} sample_t;
typedef struct {
    uint32_t jobs, invalid, misses, touch_jobs, pc_jobs;
    int64_t response_max, wall_max, latency_max, touch_max, jitter_max, previous_start;
    uint16_t firm_bits;
    uint8_t firm_count, firm_min;
    uint32_t firm_windows, firm_violations;
    sample_t worst, touch_worst, samples[N_SAMPLES];
    uint32_t samples_saved;
} stats_t;
static stats_t stats[N_TASKS];
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static bool measuring, frozen, measuring_ever;
static uint32_t run_id, requested_s, input_drops, clock_errors;
static int64_t run_start_us, run_end_us;
static uint64_t cpu_total, cpu_idle;
static uint32_t cpu_prev_total, cpu_prev_idle;
static int64_t cpu_prev_us;
static bool cpu_baseline;
static uint32_t send_errors, send_retries, disconnects, packet_seq;
static int last_errno;
static volatile bool wifi_ready;
static portMUX_TYPE time_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t sync_count, cycles_since_sync;
static int64_t last_sync_us;
static int fd = -1, client_fd = -1;
static struct sockaddr_in peer;
static socklen_t peer_len = sizeof(peer);
static bool peer_known;
static char pending[1400];
static size_t pending_len, pending_offset;
// Comandos SORT/ESTOP retransmitidos nao podem executar duas vezes.
static uint32_t seen_seq[32], seen_run[32];
static bool seen_valid[32];

static void periodic_cb(void *arg)
{
    m2_periodic_t *p = arg;
    xTaskNotifyGive(p->owner);
}

void m2_periodic_init(m2_periodic_t *p, uint32_t period_us)
{
    p->owner = xTaskGetCurrentTaskHandle();
    p->period_us = period_us;
    esp_timer_create_args_t args = {.callback = periodic_cb, .arg = p,
        .dispatch_method = ESP_TIMER_TASK, .name = "m2_periodic", .skip_unhandled_events = false};
    ESP_ERROR_CHECK(esp_timer_create(&args, &p->timer));
    // O mesmo instante absoluto e usado pelo alarme e pelo timestamp do evento.
    p->next_us = esp_timer_get_time() + period_us;
    unsigned index = period_us == 5000 ? 0 : period_us == 500000 ? 4 : 5;
    first_release_us[index] = p->next_us;
    ESP_ERROR_CHECK(esp_timer_start_periodic_at(p->timer, period_us, p->next_us));
}

int64_t m2_periodic_wait(m2_periodic_t *p)
{
    // Consome UMA liberacao, sem apagar avisos acumulados em sobrecarga.
    ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
    int64_t evt = p->next_us;
    p->next_us += p->period_us;
    return evt;
}

void m2_init(void) { /* estruturas estaticas em RAM; sem filas de logs */ }

void m2_input_drop(void)
{
    UBaseType_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
    int64_t now = esp_timer_get_time();
    if (measuring && now >= run_start_us && now < run_end_us) input_drops++;
    portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
}

void m2_log(const char *task, int64_t evt, int64_t start, int64_t end,
            uint32_t deadline, const char *origin)
{
    unsigned index;
    for (index = 0; index < N_TASKS; index++) if (!strcmp(task, names[index])) break;
    if (index == N_TASKS) return;
    // O fim ja foi capturado. Nenhuma formatacao, impressao ou rede nesta funcao.
    portENTER_CRITICAL(&stats_lock);
    // Pertence a janela pelo EVENTO, inclusive se a task iniciar atrasada.
    // Para evento invalido, usa start apenas para contabilizar a falha.
    bool outside = evt >= 0 ? evt < run_start_us || evt >= run_end_us
                            : start < run_start_us || start >= run_end_us;
    if (!measuring || outside) {
        portEXIT_CRITICAL(&stats_lock);
        return;
    }
    stats_t *s = &stats[index];
    s->jobs++;
    if (evt < 0 || evt > start || end < start || !deadline) {
        s->invalid++;
        clock_errors++;
        s->firm_count = 0; // uma janela nao atravessa uma amostra invalida
        s->firm_bits = 0;
        s->previous_start = 0;
        portEXIT_CRITICAL(&stats_lock);
        return;
    }
    sample_t record = {evt, start, end, origin};
    int64_t response = end - evt, wall = end - start, latency = start - evt;
    bool met = response <= deadline;
    if (!met) s->misses++;
    if (response > s->response_max || s->jobs == 1) { s->response_max = response; s->worst = record; }
    if (wall > s->wall_max) s->wall_max = wall;
    if (latency > s->latency_max) s->latency_max = latency;
    if (!strcmp(origin, "touch")) {
        s->touch_jobs++;
        if (latency > s->touch_max || s->touch_jobs == 1) {
            s->touch_max = latency;
            s->touch_worst = record;
        }
    }
    if (!strcmp(origin, "pc")) s->pc_jobs++;
    if (s->previous_start && periods[index]) {
        int64_t jitter = (start - s->previous_start) - periods[index];
        if (jitter < 0) jitter = -jitter;
        if (jitter > s->jitter_max) s->jitter_max = jitter;
    }
    s->previous_start = start;
    if (index >= 4) {
        s->firm_bits = ((s->firm_bits << 1) | (met ? 1 : 0)) & 1023;
        if (s->firm_count < FIRM_K) s->firm_count++;
        if (s->firm_count == FIRM_K) {
            unsigned count = 0;
            for (unsigned b = 0; b < FIRM_K; b++) count += (s->firm_bits >> b) & 1;
            s->firm_windows++;
            if (count < FIRM_M) s->firm_violations++;
            if (count < s->firm_min) s->firm_min = count;
        }
    }
    s->samples[s->samples_saved % N_SAMPLES] = record;
    s->samples_saved++;
    portEXIT_CRITICAL(&stats_lock);
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
        if (!measuring_ever) printf("M2 IP=" IPSTR " protocolo=%s porta=%d CPU=%d MHz (coleta curta v3)\n",
            IP2STR(&ip->ip_info.ip), PROTO, CONFIG_M2_PORT, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
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

static void sample_cpu(void)
{
    TaskStatus_t tasks[32];
    configRUN_TIME_COUNTER_TYPE total = 0;
    UBaseType_t count = uxTaskGetSystemState(tasks, 32, &total);
    uint32_t idle = 0;
    int64_t now = esp_timer_get_time();
    if (!count) { cpu_baseline = false; return; }
    for (UBaseType_t i = 0; i < count; i++)
        if (!strncmp(tasks[i].pcTaskName, "IDLE", 4)) idle += tasks[i].ulRunTimeCounter;
    uint32_t dt = (uint32_t)total - cpu_prev_total;
    uint32_t di = idle - cpu_prev_idle;
    if (cpu_baseline && measuring && cpu_prev_us >= run_start_us && now <= run_end_us && di <= dt) {
        cpu_total += dt;
        cpu_idle += di;
    }
    cpu_prev_total = total;
    cpu_prev_idle = idle;
    cpu_prev_us = now;
    cpu_baseline = true;
}

static void start_run(uint32_t id, uint32_t seconds)
{
    esp_log_level_set("*", ESP_LOG_NONE);
    measuring_ever = true;
    portENTER_CRITICAL(&stats_lock);
    measuring = false;
    memset(stats, 0, sizeof(stats));
    for (unsigned i = 0; i < N_TASKS; i++) stats[i].firm_min = FIRM_K;
    memset(seen_valid, 0, sizeof(seen_valid));
    input_drops = clock_errors = 0;
    send_errors = send_retries = disconnects = 0;
    last_errno = 0;
    cpu_total = cpu_idle = 0;
    cpu_baseline = false;
    run_id = id;
    requested_s = seconds;
    run_start_us = esp_timer_get_time();
    run_end_us = run_start_us + (int64_t)seconds * 1000000;
    frozen = false;
    measuring = true;
    portEXIT_CRITICAL(&stats_lock);
}

static void finish_if_due(void)
{
    // Um segundo para jobs iniciados na janela concluirem (inclui TIME D=1 s).
    if (measuring && esp_timer_get_time() >= run_end_us + 1000000) {
        portENTER_CRITICAL(&stats_lock);
        measuring = false;
        frozen = true;
        portEXIT_CRITICAL(&stats_lock);
    }
}

static bool flush_pending(void)
{
    if (!pending_len) return true;
    int n;
#if CONFIG_M2_TCP
    n = send(client_fd, pending + pending_offset, pending_len - pending_offset, 0);
#else
    n = sendto(fd, pending, pending_len, 0, (struct sockaddr *)&peer, peer_len);
#endif
    if (n < 0) {
        last_errno = errno;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOMEM || errno == ENOBUFS) {
            send_retries++;
            return false; // reter o pacote, sem descartar resultado local
        }
        send_errors++;
#if CONFIG_M2_TCP
        if (client_fd >= 0) close(client_fd);
        client_fd = -1;
        disconnects++;
#endif
        pending_len = pending_offset = 0;
        return false;
    }
    if (n == 0) { send_retries++; return false; }
    pending_offset += n;
    if (pending_offset == pending_len) pending_len = pending_offset = 0;
    return !pending_len;
}

static void packet(const char *body)
{
    uint32_t sync;
    portENTER_CRITICAL(&time_lock);
    sync = sync_count;
    portEXIT_CRITICAL(&time_lock);
    int n = snprintf(pending, sizeof(pending),
        "{\"schema\":3,\"run\":%lu,\"seq\":%lu,\"proto\":\"%s\",\"bytes\":\"0000\","
        "\"esp_ms\":%lld,\"tx_us\":%lld,\"synced\":%s,%s}\n",
        (unsigned long)run_id, (unsigned long)packet_seq++, PROTO,
        (long long)epoch_ms(), (long long)esp_timer_get_time(), sync ? "true" : "false", body);
    if (n <= 0 || n >= sizeof(pending)) { send_errors++; last_errno = EMSGSIZE; return; }
    char size[5];
    snprintf(size, sizeof(size), "%04u", (unsigned)n);
    memcpy(strstr(pending, "\"bytes\":\"") + 9, size, 4);
    pending_len = n;
    pending_offset = 0;
    flush_pending();
}

static void status_body(char *body, size_t capacity, const char *request)
{
    uint32_t sync, cycles;
    int64_t sync_us;
    portENTER_CRITICAL(&time_lock);
    sync = sync_count; cycles = cycles_since_sync; sync_us = last_sync_us;
    portEXIT_CRITICAL(&time_lock);
    double cpu = cpu_total ? 100.0 * (1.0 - (double)cpu_idle / cpu_total) : -1.0;
    snprintf(body, capacity, "%s\"type\":\"status\",\"phase\":\"%s\",\"freq_mhz\":%d,"
        "\"begin_us\":%lld,\"end_us\":%lld,\"requested_s\":%lu,\"cpu_pct\":%.3f,"
        "\"cpu_coverage_ms\":%llu,\"input_drops\":%lu,\"clock_errors\":%lu,"
        "\"send_errors\":%lu,\"send_retries\":%lu,\"last_errno\":%d,\"disconnects\":%lu,"
        "\"sync_count\":%lu,\"cycles_since_sync\":%lu,\"sync_age_ms\":%lld",
        request, frozen ? "done" : measuring ? "running" : "idle", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        (long long)run_start_us, (long long)run_end_us, (unsigned long)requested_s, cpu,
        (unsigned long long)(cpu_total / 1000), (unsigned long)input_drops, (unsigned long)clock_errors,
        (unsigned long)send_errors, (unsigned long)send_retries, last_errno,
        (unsigned long)disconnects, (unsigned long)sync, (unsigned long)cycles,
        sync ? (long long)((esp_timer_get_time() - sync_us) / 1000) : -1LL);
}

static void summary_body(char *body, size_t capacity, unsigned index, const char *request)
{
    stats_t s;
    portENTER_CRITICAL(&stats_lock);
    s = stats[index];
    portEXIT_CRITICAL(&stats_lock);
    sample_t w = s.worst, t = s.touch_worst;
    // Contagem nominal independente dos logs, para detectar jobs ainda ausentes.
    // CTRL e encadeada; SORT/SAFETY dependem de eventos, portanto expected=-1.
    int64_t expected = -1;
    if (index == 0 || index >= 4) {
        int64_t first = first_release_us[index], period = periods[index];
        if (run_start_us > first) first += ((run_start_us-first+period-1)/period)*period;
        expected = first < run_end_us ? (run_end_us-1-first)/period+1 : 0;
    }
    snprintf(body, capacity, "%s\"type\":\"summary\",\"task\":\"%s\",\"final\":%s,"
        "\"jobs\":%lu,\"expected_jobs\":%lld,\"invalid\":%lu,\"misses\":%lu,\"response_us\":%lld,"
        "\"wall_us\":%lld,\"latency_us\":%lld,\"touch_us\":%lld,\"jitter_us\":%lld,"
        "\"touch_jobs\":%lu,\"pc_jobs\":%lu,\"firm_m\":9,\"firm_k\":10,"
        "\"firm_windows\":%lu,\"firm_violations\":%lu,\"firm_min\":%u,"
        "\"worst\":[%lld,%lld,%lld,\"%s\"],\"touch_worst\":[%lld,%lld,%lld],"
        "\"sample_total\":%lu,\"sample_retained\":%lu",
        request, names[index], frozen ? "true" : "false", (unsigned long)s.jobs, (long long)expected,
        (unsigned long)s.invalid, (unsigned long)s.misses, (long long)s.response_max,
        (long long)s.wall_max, (long long)s.latency_max, (long long)s.touch_max,
        (long long)s.jitter_max, (unsigned long)s.touch_jobs, (unsigned long)s.pc_jobs,
        (unsigned long)s.firm_windows, (unsigned long)s.firm_violations, s.firm_min,
        (long long)w.evt, (long long)w.start, (long long)w.end, w.origin ? w.origin : "none",
        (long long)t.evt, (long long)t.start, (long long)t.end,
        (unsigned long)s.samples_saved,
        (unsigned long)(s.samples_saved < N_SAMPLES ? s.samples_saved : N_SAMPLES));
}

static void samples_body(char *body, size_t capacity, unsigned index, const char *request)
{
    stats_t s;
    portENTER_CRITICAL(&stats_lock);
    s = stats[index];
    portEXIT_CRITICAL(&stats_lock);
    unsigned count = s.samples_saved < N_SAMPLES ? s.samples_saved : N_SAMPLES;
    unsigned first = s.samples_saved < N_SAMPLES ? 0 : s.samples_saved % N_SAMPLES;
    size_t used = snprintf(body, capacity, "%s\"type\":\"samples\",\"task\":\"%s\",\"final\":%s,\"records\":[",
        request, names[index], frozen ? "true" : "false");
    for (unsigned i = 0; i < count; i++) {
        sample_t *v = &s.samples[(first + i) % N_SAMPLES];
        int n = snprintf(body + used, capacity - used, "%s[%lld,%lld,%lld,\"%s\"]",
            i ? "," : "", (long long)v->evt, (long long)v->start, (long long)v->end, v->origin);
        if (n <= 0 || n >= capacity - used - 1) { snprintf(body, capacity, "\"type\":\"error\",\"error\":\"sample_size\""); return; }
        used += n;
    }
    snprintf(body + used, capacity - used, "]");
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
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(CONFIG_M2_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto fail;
#if CONFIG_M2_TCP
    if (listen(fd, 1) < 0) goto fail;
#endif
    return true;
fail:
    close(fd); fd = -1; return false;
}

static void handle_command(char *line)
{
    unsigned long seq, arg, id;
    long long pc_ms;
    char cmd[16], extra;
    if (sscanf(line, "%lu,%lld,%15[^,],%lu,%lu %c", &seq, &pc_ms, cmd, &arg, &id, &extra) != 5) return;
    char request[180], body[1180];
    snprintf(request, sizeof(request), "\"cmd_seq\":%lu,\"pc_ms\":%lld,\"cmd\":\"%s\",\"rx_us\":%lld,",
        seq, pc_ms, cmd, (long long)esp_timer_get_time());
    if (!strcmp(cmd, "START")) {
        if (!id || arg < 12 || arg > 300 || (measuring && id != run_id)) {
            snprintf(body, sizeof(body), "%s\"type\":\"error\",\"error\":\"busy_or_duration\"", request);
            packet(body); return;
        }
        // Mesmo run: reenvia confirmacao sem zerar dados/estender duracao.
        if (id != run_id) start_run(id, arg);
    } else if (strcmp(cmd, "INFO") && id != run_id) {
        snprintf(body, sizeof(body), "%s\"type\":\"error\",\"error\":\"wrong_run\"", request);
        packet(body); return;
    }
    if (!strcmp(cmd, "STOP") && measuring) {
        portENTER_CRITICAL(&stats_lock);
        run_end_us = esp_timer_get_time();
        portEXIT_CRITICAL(&stats_lock);
    }
    if (!strcmp(cmd, "SORT") || !strcmp(cmd, "ESTOP")) {
        unsigned slot = seq % 32;
        if (measuring && esp_timer_get_time() < run_end_us &&
            !(seen_valid[slot] && seen_seq[slot] == seq && seen_run[slot] == id)) {
            seen_valid[slot] = true; seen_seq[slot] = seq; seen_run[slot] = id;
            m2_command(cmd, esp_timer_get_time());
        }
    }
    if (!strcmp(cmd, "GET") && arg < N_TASKS) summary_body(body, sizeof(body), arg, request);
    else if (!strcmp(cmd, "SAMPLES") && arg < N_TASKS) samples_body(body, sizeof(body), arg, request);
    else if (!strcmp(cmd, "INFO") || !strcmp(cmd, "START") || !strcmp(cmd, "STOP")) status_body(body, sizeof(body), request);
    else snprintf(body, sizeof(body), "%s\"type\":\"ack\"", request);
    packet(body);
}

static void task_time(void *arg)
{
    bool started = false;
    m2_periodic_t period;
    m2_periodic_init(&period, 1000000);
    for (;;) {
        int64_t evt = m2_periodic_wait(&period), start = esp_timer_get_time();
        if (!started && wifi_ready) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_set_time_sync_notification_cb(sync_cb);
            esp_sntp_setservername(0, CONFIG_M2_SNTP_SERVER);
            esp_sntp_set_sync_interval(CONFIG_M2_SNTP_INTERVAL_MS);
            esp_sntp_init();
            started = true;
        }
        struct timeval tv;
        gettimeofday(&tv, NULL);
        portENTER_CRITICAL(&time_lock);
        cycles_since_sync++;
        portEXIT_CRITICAL(&time_lock);
        int64_t end = esp_timer_get_time();
        m2_log("TIME", evt, start, end, 1000000, "periodic");
    }
}

static void task_monitor(void *arg)
{
    wifi_init();
    m2_periodic_t period;
    m2_periodic_init(&period, 500000);
    char rx[128];
    size_t used = 0;
    unsigned summary_index = 0, cycle = 0;
    for (;;) {
        int64_t evt = m2_periodic_wait(&period), start = esp_timer_get_time();
        finish_if_due();
        sample_cpu();
        if (wifi_ready && fd < 0) open_socket();
#if CONFIG_M2_TCP
        if (fd >= 0 && client_fd < 0) {
            client_fd = accept(fd, NULL, NULL);
            if (client_fd >= 0) { fcntl(client_fd, F_SETFL, O_NONBLOCK); used = 0; }
        }
        int sock = client_fd;
#else
        int sock = fd;
#endif
        bool replied = false;
        // Uma requisicao por ciclo basta para comandos de 1 Hz e consultas.
        if (sock >= 0 && flush_pending()) {
#if CONFIG_M2_TCP
            int n = -1;
            while (used < sizeof(rx) - 1) {
                n = recv(sock, rx + used, 1, 0);
                if (n <= 0) break;
                if (rx[used++] == '\n') break;
            }
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(client_fd); client_fd = -1; used = 0; disconnects++;
            }
#else
            int n = recvfrom(sock, rx, sizeof(rx)-1, 0, (struct sockaddr *)&peer, &peer_len);
            if (n > 0) { used = n; peer_known = true; }
#endif
            if (used && (rx[used-1] == '\n' || used == sizeof(rx)-1)) {
                rx[used] = 0;
                handle_command(rx);
                replied = true;
                used = 0;
            }
        }
#if CONFIG_M2_TCP
        bool connected = client_fd >= 0;
#else
        bool connected = fd >= 0 && peer_known;
#endif
        // Durante a medicao, um resumo de uma task por ciclo; status a cada 1 s.
        // Resultados finais so mudam em um novo START, podendo ser buscados de novo.
        if (connected && measuring && !pending_len) {
            char body[1180];
            summary_body(body, sizeof(body), summary_index, "");
            summary_index = (summary_index + 1) % N_TASKS;
            packet(body);
        }
        if (connected && measuring && !pending_len && (++cycle % 2 == 0) && !replied) {
            char body[1180];
            status_body(body, sizeof(body), "");
            packet(body);
        }
        int64_t end = esp_timer_get_time();
        m2_log("MONITOR", evt, start, end, 500000, "periodic");
    }
}

void m2_start(void)
{
    BaseType_t t = xTaskCreatePinnedToCore(task_time, "TIME", 4096, NULL, 1, NULL, 0);
    BaseType_t m = xTaskCreatePinnedToCore(task_monitor, "MONITOR", 8192, NULL, 1, NULL, 0);
    configASSERT(t == pdPASS && m == pdPASS);
    (void)t; (void)m;
}
