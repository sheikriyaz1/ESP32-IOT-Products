/**
 * @file iot_tasks.c
 * @copyright Copyright (c) 2026 Embedded AI Design Labs Pvt Ltd.
 * @author    Muhammad Samiullah, CTO & Founder
 * @brief Platform task set and ISR->task->queue data path.
 *
 * Data path:
 *
 *   Sensor task
 *        |
 *        v
 *     sample_q
 *        |
 *        v
 *     Proc task
 *       / \
 *      /   \
 *     v     v
 *  BLE FF02 telemetry_q
 *             |
 *             v
 *          MQTT pump
 *
 * BLE telemetry is sent directly from the processing task so that
 * BLE works even when Wi-Fi/MQTT is not provisioned.
 */

#include "iot_tasks.h"
#include "iot_log.h"
#include "iot_ble.h"

#include <string.h>

static const char *TAG = "iot_tasks";

static iot_task_ctx_t s_ctx;
static bool s_started;
static volatile bool s_shutdown;

typedef iot_err_t (*iot_sensor_hook_t)(iot_telemetry_msg_t *out);
typedef iot_err_t (*iot_proc_hook_t)(
    const iot_telemetry_msg_t *in,
    iot_telemetry_msg_t *out
);
typedef iot_err_t (*iot_cmd_hook_t)(const iot_command_msg_t *cmd);

static iot_sensor_hook_t s_sensor_hook;
static iot_proc_hook_t s_proc_hook;
static iot_cmd_hook_t s_cmd_hook;

static uint32_t s_seq;


/* --------------------------------------------------------------------------
 * Hook configuration
 * -------------------------------------------------------------------------- */

void iot_tasks_set_hooks(
    iot_sensor_hook_t sensor,
    iot_proc_hook_t proc,
    iot_cmd_hook_t cmd)
{
    s_sensor_hook = sensor;
    s_proc_hook = proc;
    s_cmd_hook = cmd;
}


/* --------------------------------------------------------------------------
 * Task context
 * -------------------------------------------------------------------------- */

iot_task_ctx_t *iot_tasks_ctx(void)
{
    return &s_ctx;
}


/* --------------------------------------------------------------------------
 * Shutdown request
 * -------------------------------------------------------------------------- */

void iot_tasks_request_shutdown(void)
{
    s_shutdown = true;

    if (s_ctx.system_evt != NULL) {
        iot_event_set(s_ctx.system_evt, IOT_EVT_SHUTDOWN);
    }
}


/* --------------------------------------------------------------------------
 * Sensor task
 * -------------------------------------------------------------------------- */

static void sensor_task(void *arg)
{
    (void)arg;

    iot_telemetry_msg_t msg;

    IOT_LOGI(TAG, "sensor task start");

    for (;;) {

        if (s_shutdown) {
            break;
        }

        /*
         * Wait for an ISR notification.
         *
         * If there is no physical sensor ISR, wake every 200 ms.
         * This allows the simulation/development system to continue
         * producing ADC samples.
         */
        (void)iot_sem_take(
            s_ctx.sensor_isr_sem,
            1000U
        );

        (void)memset(&msg, 0, sizeof(msg));

        msg.seq = ++s_seq;
        msg.unix_ms = (int64_t)iot_millis();

        iot_err_t e = IOT_OK;

        /*
         * Call the currently selected product profile.
         *
         * For the current project this is:
         *
         * iot_profile_energy_sensor()
         */
        if (s_sensor_hook != NULL) {

            e = s_sensor_hook(&msg);

        } else {

            /*
             * Default empty telemetry.
             */
            msg.payload_len = 2U;
            msg.payload[0] = '{';
            msg.payload[1] = '}';
        }

        if (IOT_SUCCESS(e)) {

            if (iot_queue_send(
                    s_ctx.sample_q,
                    &msg,
                    10U) != IOT_OK) {

                IOT_LOGW(
                    TAG,
                    "sample queue full - drop seq=%u",
                    (unsigned)msg.seq
                );
            }
        }
    }

    IOT_LOGI(TAG, "sensor task stop");
}


/* --------------------------------------------------------------------------
 * Processing task
 *
 * Receives samples from sample_q.
 *
 * The processed telemetry is:
 *
 *   1. Sent to BLE FF02
 *   2. Sent to telemetry_q for MQTT
 *
 * BLE works even when the device is unprovisioned.
 * -------------------------------------------------------------------------- */

static void proc_task(void *arg)
{
    (void)arg;

    iot_telemetry_msg_t in;
    iot_telemetry_msg_t out;

    IOT_LOGI(TAG, "processing task start");

    for (;;) {

        if (s_shutdown) {
            break;
        }

        if (iot_queue_recv(
                s_ctx.sample_q,
                &in,
                250U) != IOT_OK) {

            continue;
        }

        /*
         * Start with input as output.
         */
        out = in;

        /*
         * Run active profile processing hook.
         */
        if (s_proc_hook != NULL) {
            (void)s_proc_hook(&in, &out);
        }

        /* --------------------------------------------------------------
         * BLE TELEMETRY
         *
         * FF02 is the sensor characteristic.
         *
         * The BLE module already limits the payload to its cache size.
         * -------------------------------------------------------------- */

        if (out.payload_len > 0U) {

            iot_err_t ble_e = iot_ble_notify_sensor(
                out.payload,
                out.payload_len
            );

            if (IOT_FAIL(ble_e)) {

                /*
                 * Do not treat BLE-not-connected as a fatal error.
                 *
                 * BLE may simply not be connected right now.
                 */
                IOT_LOGD(
                    TAG,
                    "BLE telemetry notify skipped err=%d",
                    (int)ble_e
                );
            }
        }

        /* --------------------------------------------------------------
         * MQTT TELEMETRY QUEUE
         *
         * MQTT consumes telemetry_q when Wi-Fi/MQTT is running.
         *
         * When the device is unprovisioned, MQTT is not running, so
         * this queue can become full. In that case simply drop the
         * old telemetry instead of continuously printing warnings.
         * -------------------------------------------------------------- */

        if (iot_queue_send(
                s_ctx.telemetry_q,
                &out,
                0U) != IOT_OK) {

            /*
             * Queue full is not a fatal error.
             *
             * BLE telemetry has already been delivered above.
             */
            static uint32_t drop_count;

            drop_count++;

            /*
             * Print only every 50 drops to avoid flooding
             * the serial monitor.
             */
            if ((drop_count % 50U) == 0U) {

                IOT_LOGW(
                    TAG,
                    "telemetry queue full - dropped %u messages",
                    (unsigned)drop_count
                );
            }
        }
    }

    IOT_LOGI(TAG, "processing task stop");
}


/* --------------------------------------------------------------------------
 * Command task
 * -------------------------------------------------------------------------- */

static void cmd_task(void *arg)
{
    (void)arg;

    iot_command_msg_t cmd;

    IOT_LOGI(TAG, "command task start");

    for (;;) {

        if (s_shutdown) {
            break;
        }

        if (iot_queue_recv(
                s_ctx.command_q,
                &cmd,
                250U) != IOT_OK) {

            continue;
        }

        if (s_cmd_hook != NULL) {

    IOT_LOGI(TAG,
             "CMD received opcode=0x%04X len=%u payload=%.*s",
             (unsigned)cmd.opcode,
             (unsigned)cmd.payload_len,
             (int)cmd.payload_len,
             (const char *)cmd.payload);

    (void)s_cmd_hook(&cmd);
}
    }

    IOT_LOGI(TAG, "command task stop");
}


/* --------------------------------------------------------------------------
 * Diagnostics task
 * -------------------------------------------------------------------------- */

static void diag_task(void *arg)
{
    (void)arg;

    IOT_LOGI(TAG, "diagnostics task start");

    for (;;) {

        if (s_shutdown) {
            break;
        }

        iot_task_delay_ms(5000U);

        /*
         * Diagnostics snapshot is maintained by iot_diag.
         */
    }

    IOT_LOGI(TAG, "diagnostics task stop");
}


/* --------------------------------------------------------------------------
 * Watchdog task
 *
 * IMPORTANT:
 *
 * Only this task subscribes to the ESP-IDF Task Watchdog.
 *
 * app_main() must NOT call iot_wdt_add_current().
 * -------------------------------------------------------------------------- */

static void wdt_task(void *arg)
{
    (void)arg;

    /*
     * These functions are implemented by iot_wdt.c.
     */
    extern iot_err_t iot_wdt_add_current(void);
    extern iot_err_t iot_wdt_reset(void);

    /*
     * Subscribe THIS task to the watchdog.
     */
    iot_err_t e = iot_wdt_add_current();

    if (IOT_FAIL(e)) {

        IOT_LOGE(
            TAG,
            "failed to subscribe wdt task, err=%d",
            (int)e
        );

        return;
    }

    IOT_LOGI(TAG, "watchdog monitor task start");

    for (;;) {

        if (s_shutdown) {
            break;
        }

        /*
         * Feed watchdog.
         */
        (void)iot_wdt_reset();

        /*
         * Feed every 2 seconds.
         */
        iot_task_delay_ms(2000U);
    }

    IOT_LOGI(TAG, "watchdog task stop");
}


/* --------------------------------------------------------------------------
 * Start task architecture
 * -------------------------------------------------------------------------- */

iot_err_t iot_tasks_start(iot_task_ctx_t *ctx)
{
    if (s_started) {
        return IOT_ERR_ALREADY_INIT;
    }

    /*
     * Reset task state.
     */
    s_shutdown = false;
    s_seq = 0U;


    /* ----------------------------------------------------------------------
     * Sample queue
     * ---------------------------------------------------------------------- */

    iot_err_t e = iot_queue_create(
        16U,
        (uint32_t)sizeof(iot_telemetry_msg_t),
        &s_ctx.sample_q
    );

    if (IOT_FAIL(e)) {
        return e;
    }


    /* ----------------------------------------------------------------------
     * Telemetry queue
     * ---------------------------------------------------------------------- */

    e = iot_queue_create(
        16U,
        (uint32_t)sizeof(iot_telemetry_msg_t),
        &s_ctx.telemetry_q
    );

    if (IOT_FAIL(e)) {
        return e;
    }


    /* ----------------------------------------------------------------------
     * Command queue
     * ---------------------------------------------------------------------- */

    e = iot_queue_create(
        8U,
        (uint32_t)sizeof(iot_command_msg_t),
        &s_ctx.command_q
    );

    if (IOT_FAIL(e)) {
        return e;
    }


    /* ----------------------------------------------------------------------
     * Sensor semaphore
     * ---------------------------------------------------------------------- */

    e = iot_sem_binary_create(
        &s_ctx.sensor_isr_sem
    );

    if (IOT_FAIL(e)) {
        return e;
    }


    /* ----------------------------------------------------------------------
     * Sample mutex
     * ---------------------------------------------------------------------- */

    e = iot_mutex_create(
        &s_ctx.sample_lock
    );

    if (IOT_FAIL(e)) {
        return e;
    }


    /* ----------------------------------------------------------------------
     * System event group
     * ---------------------------------------------------------------------- */

    e = iot_event_create(
        &s_ctx.system_evt
    );

    if (IOT_FAIL(e)) {
        return e;
    }


    /*
     * Kick the first sample immediately.
     *
     * This is important because we don't have a physical ADC ISR
     * generating the first event.
     */
    (void)iot_sem_give(
        s_ctx.sensor_isr_sem
    );


    /* ----------------------------------------------------------------------
     * Sensor task
     * ---------------------------------------------------------------------- */

    e = iot_task_create(
        "sensor",
        sensor_task,
        NULL,
        8192U,
        IOT_PRIO_SENSOR,
        &s_ctx.sensor_task
    );

    if (IOT_FAIL(e)) {
        return e;
    }


    /* ----------------------------------------------------------------------
     * Processing task
     * ---------------------------------------------------------------------- */

    e = iot_task_create(
        "proc",
        proc_task,
        NULL,
        8192U,
        IOT_PRIO_PROC,
        &s_ctx.proc_task
    );

    if (IOT_FAIL(e)) {
        return e;
    }


    /* ----------------------------------------------------------------------
     * Command task
     * ---------------------------------------------------------------------- */

    e = iot_task_create(
        "cmd",
        cmd_task,
        NULL,
        8192U,
        IOT_PRIO_CMD,
        &s_ctx.cmd_task
    );

    if (IOT_FAIL(e)) {
        return e;
    }


    /* ----------------------------------------------------------------------
     * Diagnostics task
     * ---------------------------------------------------------------------- */

    e = iot_task_create(
        "diag",
        diag_task,
        NULL,
        3072U,
        IOT_PRIO_DIAG,
        &s_ctx.diag_task
    );

    if (IOT_FAIL(e)) {
        return e;
    }


    /* ----------------------------------------------------------------------
     * Watchdog task
     * ---------------------------------------------------------------------- */

    e = iot_task_create(
        "wdt",
        wdt_task,
        NULL,
        2048U,
        IOT_PRIO_WDT,
        &s_ctx.wdt_task
    );

    if (IOT_FAIL(e)) {
        return e;
    }


    /* ----------------------------------------------------------------------
     * Task architecture ready
     * ---------------------------------------------------------------------- */

    s_started = true;

    if (ctx != NULL) {
        *ctx = s_ctx;
    }

    IOT_LOGI(
        TAG,
        "task architecture started"
    );

    return IOT_OK;
}