/*
Projeto ESP-32 - Guilherme Santana e Thiago Bazilio
*/

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "m2.h"

// Esteira Industrial (ESP32 + FreeRTOS) — M1 + MONITOR e TIME da M2
// - ENC_SENSE (periódica 5 ms) -> notifica SPD_CTRL
// - SPD_CTRL (hard RT) -> controle PI simulado, trata HMI (soft) se solicitado
// - SORT_ACT (hard RT, evento Touch B) -> aciona "desviador"
// - SAFETY_TASK (hard RT, evento Touch D) -> E-stop
// ESP-IDF 6.1: M2 usa timer com alarme absoluto

#include <string.h>
#include <stdbool.h>
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/touch_sensor.h"  // mudei tive que mudar, ta obsoleta e n tava rodando...


// ====== Mapeamento dos touch pads ======
#define TP_OBJ   TOUCH_PAD_NUM7   // Touch B -> detecção de objeto (desvio) -> sort_act
#define TP_HMI   TOUCH_PAD_NUM8   // Touch C -> HMI/telemetria
#define TP_ESTOP TOUCH_PAD_NUM9   // Touch D -> E-stop                      -> safety_task

// ====== Periodicidade, prioridades, stack ======
#define ENC_T_MS        5
#define PRIO_ESTOP      5
#define PRIO_ENC        3
#define PRIO_CTRL       2
#define PRIO_SORT       4
#define STK             3072
#define ENC_D_US        5000 // deadline enc
#define CTRL_D_US       10000 // deadline ctrl
#define SORT_D_US       10000 // deadline sort
#define ESTOP_D_US      5000 // deadline estop
// us é micro s


// ====== Handles/IPC ======
static TaskHandle_t hENC = NULL, hCTRL = NULL, hSORT = NULL, hSAFE = NULL;

// Notificação ENC->CTRL (direct-to-task notification)
static TaskHandle_t hCtrlNotify = NULL;

// Fila de eventos para o SORT_ACT
typedef struct {
    int64_t t_evt_us;   // timestamp do toque (para logging/latência)
    const char *origem;
} sort_evt_t;
static QueueHandle_t qSort = NULL;

// Semáforos para E-stop e HMI
static QueueHandle_t qEStop = NULL; // M2: preserva timestamp/origem de cada evento
static SemaphoreHandle_t semHMI   = NULL;

// contadores para os testes
static volatile int64_t  tCtrlEvtUs = 0;
static portMUX_TYPE ctrl_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t  enc_evt_us = 0, enc_start_us = 0, enc_end_us = 0;
static volatile int64_t  ctrl_evt_us = 0, ctrl_start_us = 0, ctrl_end_us = 0;
static volatile uint32_t enc_misses = 0, ctrl_misses = 0, sort_misses = 0, estop_misses = 0;
static volatile uint32_t enc_jobs = 0, ctrl_jobs = 0, sort_jobs = 0, estop_jobs = 0;
static volatile bool     g_estop = false;

// Estado "simulado" da esteira
typedef struct {
    float rpm;
    float pos_mm;
    float set_rpm;
    float pwm;
    uint32_t producao;
} belt_state_t;

static belt_state_t g_belt = { 
    .rpm = 0.f, 
    .pos_mm = 0.f,
    .set_rpm = 120.0f, 
    .pwm = 0.f,
    .producao = 0 };

// ====== Util: busy loop previsível (~WCET) ======
static inline void cpu_tight_loop_us(uint32_t us)
{
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < us) {
        __asm__ __volatile__("nop");
    }
}


// ====== ISR dos touch pads ======
static void IRAM_ATTR touch_isr(void *arg) {
    uint32_t status = touch_pad_get_status();
    BaseType_t acordou_task = pdFALSE;
    int64_t agora = esp_timer_get_time();

    touch_pad_clear_status();

    if (status & (1UL << TP_OBJ)) {
        sort_evt_t ev = { .t_evt_us = agora, .origem = "touch" };
        if (xQueueSendFromISR(qSort, &ev, &acordou_task) != pdTRUE) m2_input_drop();
    }

    if (status & (1UL << TP_HMI)) {
        xSemaphoreGiveFromISR(semHMI, &acordou_task);
    }

    if (status & (1UL << TP_ESTOP)) {
        sort_evt_t ev = { .t_evt_us = agora, .origem = "touch" };
        if (xQueueSendFromISR(qEStop, &ev, &acordou_task) != pdTRUE) m2_input_drop();
    }

    if (acordou_task == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// ====== Inicialização dos touch pads ======
static void touch_init(void) {
    uint16_t touch_value;

    touch_pad_init();
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    touch_pad_set_trigger_mode(TOUCH_TRIGGER_BELOW);

    touch_pad_config(TP_OBJ, 0);
    touch_pad_config(TP_HMI, 0);
    touch_pad_config(TP_ESTOP, 0);

    vTaskDelay(pdMS_TO_TICKS(50));

    touch_pad_read(TP_OBJ, &touch_value);
    touch_pad_set_thresh(TP_OBJ, touch_value * 80 / 100);

    touch_pad_read(TP_HMI, &touch_value);
    touch_pad_set_thresh(TP_HMI, touch_value * 80 / 100);

    touch_pad_read(TP_ESTOP, &touch_value);
    touch_pad_set_thresh(TP_ESTOP, touch_value * 80 / 100);

    touch_pad_isr_register(touch_isr, NULL);
    touch_pad_intr_enable();
}

//
// ====== ENC_SENSE (periódica 5 ms): estima velocidade/posição ======
static void task_enc_sense(void *arg)
{
    m2_periodic_t periodo;
    m2_periodic_init(&periodo, ENC_T_MS * 1000);

    for (;;) {
        enc_evt_us = m2_periodic_wait(&periodo);
        enc_start_us = esp_timer_get_time();
        enc_jobs++;

        // Dinâmica simulada: aproxima rpm do setpoint, integra posição
        float rpm_motor = (g_belt.pwm / 100.0f) * 200.0f;
        float err = rpm_motor - g_belt.rpm;
        g_belt.rpm += 0.05f * err;             // aproximação lenta para simular inércia
        g_belt.pos_mm += (g_belt.rpm / 60.0f) * (ENC_T_MS / 1000.0f) * 100.0f; // 100 mm por rev (exemplo)

        // Carga determinística ~0.7 ms
        cpu_tight_loop_us(700);

        enc_end_us = esp_timer_get_time();
        if ((enc_end_us - enc_evt_us) > ENC_D_US) enc_misses++;

        // Notifica controle
        if (hCtrlNotify) {
            portENTER_CRITICAL(&ctrl_lock);
            tCtrlEvtUs = enc_end_us;
            portEXIT_CRITICAL(&ctrl_lock);
            xTaskNotifyGive(hCtrlNotify);
        }

        m2_log("ENC", enc_evt_us, enc_start_us, enc_end_us, ENC_D_US, "periodic");
    }
}

// ====== SPD_CTRL (encadeada): controle PI simulado + HMI (soft) ======
static void task_spd_ctrl(void *arg)
{
    // um controlador PI minimalista e simulado
    float kp = 0.4f, ki = 0.1f, integ = 0.f;

    for (;;) {
        uint32_t avisos = ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // acorda após ENC_SENSE
        portENTER_CRITICAL(&ctrl_lock);
        // ENC pode ter preemptado CTRL entre o despertar e esta secao.
        // Consome avisos adicionais antes de capturar o timestamp mais recente.
        avisos += ulTaskNotifyTake(pdTRUE, 0);
        ctrl_evt_us = tCtrlEvtUs;
        portEXIT_CRITICAL(&ctrl_lock);
        while (avisos-- > 1) m2_input_drop(); // notificacoes coalescidas sao perdas
        ctrl_start_us = esp_timer_get_time();
        ctrl_jobs++;

        // Controle (hard): alvo é g_belt.set_rpm, atuando em g_belt.rpm (simulado)
        if (!g_estop) {
            float err = g_belt.set_rpm - g_belt.rpm;
            integ += err * (ENC_T_MS / 1000.0f);
            float u = kp * err + ki * integ;
            
            // muito mock
            if (u > 100.0f) u = 100.0f;
            if (u < 0.0f)   u = 0.0f;
            // M2: SAFETY pode preemptar o PI; nao restaurar PWM apos E-stop.
            portENTER_CRITICAL(&ctrl_lock);
            g_belt.pwm = g_estop ? 0.f : u;
            portEXIT_CRITICAL(&ctrl_lock);

        } else {
            g_belt.pwm = 0.f;
            integ = 0.f;
        }

        // Carga determinística ~1.2 ms
        cpu_tight_loop_us(1200);

        // Trecho nao-critico (soft): mantem a carga simulada da HMI,
        // sem imprimir. Tempos e contadores ficam em memoria para o PC.
        if (xSemaphoreTake(semHMI, 0) == pdTRUE) {
            cpu_tight_loop_us(400); // ~0.4 ms (soft)
        }
        // Captura fim ANTES de enfileirar qualquer registro.
        ctrl_end_us = esp_timer_get_time();
        if ((ctrl_end_us - ctrl_evt_us) > CTRL_D_US) ctrl_misses++;
        m2_log("CTRL", ctrl_evt_us, ctrl_start_us, ctrl_end_us, CTRL_D_US, "encoder");
    }
}

// ====== SORT_ACT (evento Touch B): aciona "desviador" no tempo certo ======
static void task_sort_act(void *arg)
{
    sort_evt_t ev;
    for (;;) {
        if (xQueueReceive(qSort, &ev, portMAX_DELAY) == pdTRUE) {
            int64_t t0 = esp_timer_get_time();
            sort_jobs++;

            // Preparação de janela crítica (ex.: 0.7 ms)
            cpu_tight_loop_us(700);

            // Aqui você acionaria GPIO/solenóide no "instante"
            // Nesta demo, apenas logamos o tempo gasto:
            g_belt.producao++;
            int64_t t1 = esp_timer_get_time();
            if ((t1 - ev.t_evt_us) > SORT_D_US) sort_misses++;

            m2_log("SORT", ev.t_evt_us, t0, t1, SORT_D_US, ev.origem);
        }
    }
}

//
// ====== SAFETY_TASK (evento Touch D): E-stop ======
static void task_safety(void *arg)
{
    sort_evt_t ev;
    for (;;) {
        if (xQueueReceive(qEStop, &ev, portMAX_DELAY) == pdTRUE) {
            int64_t t_evt = ev.t_evt_us;
            int64_t t0 = esp_timer_get_time();
            estop_jobs++;

            // Ação crítica: zera "PWM", trava atuadores, sinaliza alarme (~0.9 ms)
            g_estop = true;
            g_belt.pwm = 0.f;
            if (CONFIG_M2_LED_GPIO >= 0) gpio_set_level(CONFIG_M2_LED_GPIO, 1); // ACENDE O LED
            cpu_tight_loop_us(900);

            int64_t t1 = esp_timer_get_time();
            if ((t1 - t_evt) > ESTOP_D_US) estop_misses++;

            m2_log("SAFETY", t_evt, t0, t1, ESTOP_D_US, ev.origem);
        }
    }
}


// M2: mesmos caminhos das tasks de evento, agora tambem acionados pelo PC.
void m2_command(const char *cmd, int64_t evt)
{
    sort_evt_t ev = { .t_evt_us = evt, .origem = "pc" };
    QueueHandle_t q = !strcmp(cmd, "SORT") ? qSort :
                      !strcmp(cmd, "ESTOP") ? qEStop : NULL;
    if (q && xQueueSend(q, &ev, 0) != pdTRUE) m2_input_drop();
}

// ====== app_main ======
void app_main(void)
{

    qSort = xQueueCreate(10, sizeof(sort_evt_t));
    qEStop = xQueueCreate(10, sizeof(sort_evt_t));
    semHMI = xSemaphoreCreateBinary();

    configASSERT(qSort && qEStop && semHMI);
    m2_init();
    if (CONFIG_M2_LED_GPIO >= 0) {
        gpio_set_direction(CONFIG_M2_LED_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(CONFIG_M2_LED_GPIO, 0); //começa desligado o led
    }
    touch_init();

    // Cria tasks (afinidade no core 0 para reduzir jitter, opcional)
    xTaskCreatePinnedToCore(task_safety,   "SAFETY",   STK, NULL, PRIO_ESTOP, &hSAFE, 0);
    xTaskCreatePinnedToCore(task_spd_ctrl, "SPD_CTRL", STK, NULL, PRIO_CTRL,  &hCTRL, 0);
    hCtrlNotify = hCTRL;
    xTaskCreatePinnedToCore(task_enc_sense,"ENC_SENSE",STK, NULL, PRIO_ENC,   &hENC,  0);
    xTaskCreatePinnedToCore(task_sort_act, "SORT_ACT", STK, NULL, PRIO_SORT,  &hSORT, 0);
    m2_start();

}
