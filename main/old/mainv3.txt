/* Instrumentation, Wi-Fi transport and UTC synchronization.
 * All socket operations are nonblocking; only MONITOR_TASK uses sockets. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_sntp.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "m2.h"

#define LOG_LEN 512
#define PACKET_LEN 1300
static const int64_t deadlines[M2_TASKS] = {5000,10000,10000,5000,500000,1000000,500000};
typedef struct {
    uint32_t task, source, job;
    int64_t evt, start, end, block;
} record_t;
typedef struct { uint32_t jobs, misses, history, windows, violations; int64_t wcrt; } stats_t;
static stats_t stats[M2_TASKS];
static uint32_t drops[M2_TASKS], log_drops, tx_drops;
static QueueHandle_t logs, replies;
static QueueHandle_t releases[M2_TASKS];
static esp_timer_handle_t periodic_timers[M2_TASKS];
static EventGroupHandle_t wifi_events;
static portMUX_TYPE stats_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE sync_mux = portMUX_INITIALIZER_UNLOCKED;
static bool synced;
static uint32_t cycle_last, sync_generation, time_iterations;
static uint64_t cycles;
static int64_t sync_mono;

/* Release timestamps are captured in the esp_timer callback, before the
 * application task becomes ready. Queueing delay remains in its response time.
 * Timer service jitter must be analyzed separately from this release latency. */

static void periodic_release(void *arg) {
    unsigned task = (unsigned)(uintptr_t)arg;
    int64_t now = esp_timer_get_time();
    if (xQueueSend(releases[task], &now, 0) != pdTRUE) m2_drop(task);
}


void m2_wait_release(unsigned task, int64_t *release) {
    xQueueReceive(releases[task], release, portMAX_DELAY);
}


void m2_start_periodic(void) {
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timers[M2_ENC], 5000));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timers[M2_MONITOR], 500000));
    if (!CONFIG_ESTEIRA_M1_BASELINE)
        ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timers[M2_TIME], 1000000));
}


void IRAM_ATTR m2_drop_isr(unsigned task) {
    portENTER_CRITICAL_ISR(&stats_mux);
    drops[task]++;
    portEXIT_CRITICAL_ISR(&stats_mux);
}


void m2_drop(unsigned task) {
    portENTER_CRITICAL(&stats_mux);
    drops[task]++;
    portEXIT_CRITICAL(&stats_mux);
}


void m2_record(unsigned task, unsigned source, int64_t evt, int64_t start, int64_t end, int64_t block) {
    record_t r = {.task=task, .source=source, .evt=evt, .start=start, .end=end, .block=block};
    portENTER_CRITICAL(&stats_mux);
    stats_t *s = &stats[task];
    r.job = ++s->jobs;
    bool pass = end - evt <= deadlines[task];
    s->misses += !pass;
    if (end - evt > s->wcrt) s->wcrt = end - evt;
    s->history = ((s->history << 1) | pass) & 1023U;
    if (s->jobs >= 10 && (task == M2_MONITOR || task == M2_TIME || task == M2_HMI)) {
        s->windows++;
        if (__builtin_popcount(s->history) < 9) s->violations++;
    }
    portEXIT_CRITICAL(&stats_mux);
    if (xQueueSend(logs, &r, 0) != pdTRUE) {
        portENTER_CRITICAL(&stats_mux);
        log_drops++;
        portEXIT_CRITICAL(&stats_mux);
    }
}


static int64_t utc_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}


static void sync_cb(struct timeval *tv) {
    (void)tv;
    portENTER_CRITICAL(&sync_mux);
    synced = true;
    cycle_last = esp_cpu_get_cycle_count();
    cycles = 0;
    time_iterations = 0;
    sync_mono = esp_timer_get_time();
    sync_generation++;
    portEXIT_CRITICAL(&sync_mux);
}


static void wifi_cb(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(wifi_events, 1);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, 1);
    }
}


void m2_init(void) {
    logs = xQueueCreate(LOG_LEN, sizeof(record_t));
    replies = xQueueCreate(32, 192);
    wifi_events = xEventGroupCreate();
    configASSERT(logs && replies && wifi_events);
    const unsigned periodic_ids[] = {M2_ENC, M2_MONITOR, M2_TIME};
    for (unsigned i = 0; i < 3; ++i) {
        unsigned id = periodic_ids[i];
        releases[id] = xQueueCreate(32, sizeof(int64_t));
        configASSERT(releases[id]);
        esp_timer_create_args_t timer = {.callback=periodic_release,
            .arg=(void *)(uintptr_t)id, .name="release"};
        ESP_ERROR_CHECK(esp_timer_create(&timer, &periodic_timers[id]));
    }
    if (CONFIG_ESTEIRA_M1_BASELINE) return;
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    configASSERT(esp_netif_create_default_wifi_sta());
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_cb, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_cb, NULL));
    wifi_config_t cfg = {0};
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", CONFIG_ESTEIRA_WIFI_SSID);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", CONFIG_ESTEIRA_WIFI_PASSWORD);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}


void m2_time_task(void *arg) {
    int64_t release;
    bool started = false;
    for (;;) {
        m2_wait_release(M2_TIME, &release);
        int64_t start = esp_timer_get_time();
        if (!started && (xEventGroupGetBits(wifi_events) & 1)) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, CONFIG_ESTEIRA_SNTP_SERVER);
            esp_sntp_set_time_sync_notification_cb(sync_cb);
            esp_sntp_init();
            started = true;
        }
        portENTER_CRITICAL(&sync_mux);
        uint32_t now = esp_cpu_get_cycle_count();
        if (synced) {
            cycles += (uint32_t)(now - cycle_last);
            ++time_iterations;
        }
        cycle_last = now;
        portEXIT_CRITICAL(&sync_mux);
        m2_record(M2_TIME, M2_PERIODIC, release, start, esp_timer_get_time(), 0);
    }
}


static void command(const char *line) {
    unsigned id;
    char verb[16], extra;
    int64_t pc_utc;
    if (sscanf(line, "CMD,%u,%15[^,],%" SCNd64 "%c", &id, verb, &pc_utc, &extra) != 3) return;
    if (strcmp(verb, "SORT") && strcmp(verb, "ESTOP") && strcmp(verb, "PING")) return;
    int64_t rx = esp_timer_get_time(), rx_utc = utc_us();
    bool ok = !strcmp(verb, "PING") || m2_activate(!strcmp(verb, "ESTOP"), M2_PC, rx);
    char reply[192] = {0};
    snprintf(reply, sizeof(reply), "A,%u,%" PRId64 ",%" PRId64 ",%" PRId64 ",%d\n",
             id, pc_utc, rx, rx_utc, ok);
    if (xQueueSend(replies, reply, 0) != pdTRUE) ++tx_drops;
}

/* Persistent TCP buffer retains partial writes across releases. UDP packets
 * contain complete lines; oversized commands are rejected. */
static int fd = -1;
static bool connecting;
static char tx[PACKET_LEN], rx[256];
static size_t tx_len, tx_pos, rx_len;
static uint32_t sequence;
static struct sockaddr_in peer;

static void disconnect_peer(void) {
    if (fd >= 0) close(fd);
    fd = -1; connecting = false; rx_len = 0;
    if (tx_len) ++tx_drops;
    tx_len = tx_pos = 0;
}


static bool socket_ready(void) {
    if (CONFIG_ESTEIRA_M1_BASELINE) return true;
    if (!(xEventGroupGetBits(wifi_events) & 1)) { disconnect_peer(); return false; }
    if (fd < 0) {
        fd = socket(AF_INET, CONFIG_ESTEIRA_TCP ? SOCK_STREAM : SOCK_DGRAM, IPPROTO_IP);
        if (fd < 0) return false;
        if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { disconnect_peer(); return false; }
        peer = (struct sockaddr_in){.sin_family=AF_INET, .sin_port=htons(CONFIG_ESTEIRA_PORT)};
        if (inet_pton(AF_INET, CONFIG_ESTEIRA_PC_IP, &peer.sin_addr) != 1) {
            disconnect_peer(); return false;
        }
        /* Connected UDP limits incoming commands to the configured PC peer. */
        int r = connect(fd, (struct sockaddr *)&peer, sizeof(peer));
        if (r < 0 && errno != EINPROGRESS) { disconnect_peer(); return false; }
        connecting = r < 0;
    }
    if (connecting) {
        fd_set wr; FD_ZERO(&wr); FD_SET(fd, &wr);
        struct timeval zero = {0};
        int ready = select(fd + 1, NULL, &wr, NULL, &zero);
        if (ready < 0) { disconnect_peer(); return false; }
        if (!ready) return false;
        int error = 0; socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error) {
            disconnect_peer(); return false;
        }
        connecting = false;
    }
    return true;
}


static void receive_commands(void) {
    /* Bounded number of receives per 500 ms activation. */
    for (unsigned n = 0; n < 32; ++n) {
        char chunk[256];
        int got = recv(fd, chunk, sizeof(chunk), 0);
        if (got < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) disconnect_peer();
            return;
        }
        if (!got) { if (CONFIG_ESTEIRA_TCP) disconnect_peer(); return; }
        for (int i = 0; i < got; ++i) {
            if (chunk[i] == '\n') {
                if (rx_len < sizeof(rx) - 1) { rx[rx_len] = 0; command(rx); }
                rx_len = 0;
            } else if (rx_len < sizeof(rx) - 1) rx[rx_len++] = chunk[i];
        }
        if (!CONFIG_ESTEIRA_TCP) rx_len = 0;
    }
}


static void make_packet(const char *body, size_t body_len) {
    bool valid;
    portENTER_CRITICAL(&sync_mux); valid = synced; portEXIT_CRITICAL(&sync_mux);
    int64_t mono = esp_timer_get_time(), utc = utc_us();
    uint32_t lost;
    portENTER_CRITICAL(&stats_mux); lost = log_drops; portEXIT_CRITICAL(&stats_mux);
    const char *format = "B,%u,%s,%04u,%" PRId64 ",%" PRId64 ",%d,%s,%d,%d,%d,1,%u,%u\n";
    unsigned seq = ++sequence;
    const char *protocol = CONFIG_ESTEIRA_M1_BASELINE ? "UART" : CONFIG_ESTEIRA_TCP ? "TCP" : "UDP";
    int h = snprintf(tx, sizeof(tx), format, seq, protocol, 0U,
        utc, mono, valid, CONFIG_ESTEIRA_DM ? "DM" : "CUSTOM", configUSE_PREEMPTION,
        configUSE_TIME_SLICING, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, lost, tx_drops);
    h = snprintf(tx, sizeof(tx), format, seq, protocol, (unsigned)(h + body_len),
        utc, mono, valid, CONFIG_ESTEIRA_DM ? "DM" : "CUSTOM", configUSE_PREEMPTION,
        configUSE_TIME_SLICING, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, lost, tx_drops);
    configASSERT(h > 0 && h + body_len < sizeof(tx));
    memcpy(tx + h, body, body_len);
    tx_len = h + body_len; tx_pos = 0;
}


static bool flush_packet(void) {
    if (CONFIG_ESTEIRA_M1_BASELINE) {
        size_t sent = fwrite(tx + tx_pos, 1, tx_len - tx_pos, stdout);
        tx_pos += sent;
        if (tx_pos < tx_len) return false;
        tx_len = tx_pos = 0;
        return true;
    }
    int sent = send(fd, tx + tx_pos, tx_len - tx_pos, 0);
    if (sent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) disconnect_peer();
        return false;
    }
    if (!sent) { disconnect_peer(); return false; }
    if (!CONFIG_ESTEIRA_TCP && (size_t)sent != tx_len) { ++tx_drops; tx_len = tx_pos = 0; return false; }
    tx_pos += sent;
    if (tx_pos < tx_len) return false;
    tx_len = tx_pos = 0;
    return true;
}


static void send_logs(void) {
    char body[1000], reply[192];
    for (unsigned batch = 0; batch < 64; ++batch) {
        if (tx_len && !flush_packet()) return;
        size_t used = 0;
        while (used < sizeof(body) - 200 && xQueueReceive(replies, reply, 0) == pdTRUE) {
            size_t len = strlen(reply); memcpy(body + used, reply, len); used += len;
        }
        record_t r;
        while (used < sizeof(body) - 200 && xQueueReceive(logs, &r, 0) == pdTRUE) {
            used += snprintf(body + used, sizeof(body) - used,
                "E,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 "\n",
                r.task, r.source, r.job, r.evt, r.start, r.end, deadlines[r.task], r.block);
        }
        if (!used) return;
        make_packet(body, used);
        if (!flush_packet()) return;
    }
}


/* Runtime counters are CPU execution time, unlike (end-start), which includes
 * preemption and blocking. Snapshot deltas handle 32-bit wrap modulo 2^32. */
static void heartbeat(void) {
    static TaskStatus_t tasks[40];
    static uint32_t previous_total, previous_idle;
    configRUN_TIME_COUNTER_TYPE total = 0;
    UBaseType_t count = uxTaskGetSystemState(tasks, 40, &total);
    uint32_t idle = 0;
    for (unsigned i = 0; i < count; ++i)
        if (!strncmp(tasks[i].pcTaskName, "IDLE", 4)) idle += tasks[i].ulRunTimeCounter;
    uint32_t dt = (uint32_t)total - previous_total, di = idle - previous_idle;
    int busy_permille = count && dt ? (int)(1000U - (uint64_t)di * 1000U / dt) : -1;
    previous_total = total; previous_idle = idle;
    uint64_t elapsed_cycles; int64_t age; uint32_t iterations, gen;
    portENTER_CRITICAL(&sync_mux);
    elapsed_cycles = cycles + (synced ? (uint32_t)(esp_cpu_get_cycle_count() - cycle_last) : 0);
    age = synced ? esp_timer_get_time() - sync_mono : -1;
    iterations = time_iterations; gen = sync_generation;
    portEXIT_CRITICAL(&sync_mux);
    char body[1000];
    int used = snprintf(body, sizeof(body), "T,%" PRId64 ",%" PRIu64 ",%" PRIu32 ",%" PRIu32 ",%d\n",
                        age, elapsed_cycles, iterations, gen, busy_permille);
    for (unsigned i = 0; i < M2_TASKS; ++i) {
        stats_t s; uint32_t dropped;
        portENTER_CRITICAL(&stats_mux); s = stats[i]; dropped = drops[i]; portEXIT_CRITICAL(&stats_mux);
        used += snprintf(body + used, sizeof(body) - used,
            "S,%u,%" PRIu32 ",%" PRIu32 ",%" PRId64 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
            i, s.jobs, s.misses, s.wcrt, s.windows, s.violations, dropped);
    }
    make_packet(body, used);
    flush_packet();
}


void m2_monitor_task(void *arg) {
    int64_t release;
    for (;;) {
        m2_wait_release(M2_MONITOR, &release);
        int64_t start = esp_timer_get_time();
        if (socket_ready()) {
            if (!CONFIG_ESTEIRA_M1_BASELINE) receive_commands();
            if (CONFIG_ESTEIRA_M1_BASELINE || fd >= 0) send_logs();
            if ((CONFIG_ESTEIRA_M1_BASELINE || fd >= 0) && !tx_len) heartbeat();
        }
        if (!CONFIG_ESTEIRA_M1_BASELINE)
            m2_record(M2_MONITOR, M2_PERIODIC, release, start, esp_timer_get_time(), 0);
    }
}
