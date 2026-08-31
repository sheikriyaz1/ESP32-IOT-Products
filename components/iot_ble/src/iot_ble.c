/**
 * @file iot_ble.c
 * @copyright Copyright (c) 2026 Embedded AI Design Labs Pvt Ltd.
 * @author    Muhammad Samiullah, CTO & Founder
 * @brief NimBLE GATT: Device Info, Config, Sensor, Control, Diagnostics, OTA status.
 *
 * Heavy work is posted to command_q.
 * GAP/GATT callbacks return immediately.
 *
 * Provisioning writes to FF01 are reassembled because a single BLE/ATT
 * packet may not contain the complete JSON payload.
 *
 * @syscalls nimble_port_*, ble_gap_adv_start, ble_gatts_*
 * @thread nimble host task + GAP event
 */

#include "iot_ble.h"
#include "iot_log.h"
#include "iot_tasks.h"
#include "iot_config.h"
#include "iot_version.h"
#include "iot_types.h"

#include <string.h>
#include <stdint.h>

#ifdef ESP_PLATFORM

#include "esp_bt.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"

#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#endif


/* --------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------- */

static const char *TAG = "iot_ble";

static iot_ble_state_t s_state = IOT_BLE_IDLE;

static bool s_connected;

static char s_name[32] = "ESP32C3-IoT";

static uint8_t s_sensor_cache[64];

static uint16_t s_sensor_len;


#ifdef ESP_PLATFORM

static uint16_t s_conn;

static uint16_t s_chr_sensor_val;


/*
 * --------------------------------------------------------------------------
 * BLE provisioning reassembly buffer
 * --------------------------------------------------------------------------
 *
 * Example incoming BLE fragments:
 *
 *   {"ssid":"R","pass":"
 *   25802580"}
 *
 * These fragments are accumulated here until the closing '}' arrives.
 *
 */

static uint8_t s_provision_buf[IOT_JSON_SMALL_LEN];

static uint16_t s_provision_len;


/* --------------------------------------------------------------------------
 * 128-bit service UUID
 * -------------------------------------------------------------------------- */

/*
 * 128-bit service UUID:
 *
 * 0000IOT1-0000-1000-8000-00805F9B34FB
 *
 * encoded little-endian.
 */

static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(
        0xfb,
        0x34,
        0x9b,
        0x5f,
        0x80,
        0x00,
        0x00,
        0x80,
        0x00,
        0x10,
        0x00,
        0x00,
        0x01,
        0x49,
        0x4f,
        0x54
    );


/* --------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------- */

static int gatt_access(
    uint16_t conn,
    uint16_t attr,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg
);


/* --------------------------------------------------------------------------
 * GATT characteristics
 * -------------------------------------------------------------------------- */

static const struct ble_gatt_chr_def s_chrs[] = {

    {
        /*
         * FF01
         *
         * Configuration characteristic.
         *
         * Used for BLE provisioning JSON:
         *
         * {"ssid":"R","pass":"25802580"}
         *
         */

        .uuid = BLE_UUID16_DECLARE(0xFF01),

        .access_cb = gatt_access,

        .flags =
            BLE_GATT_CHR_F_READ |
            BLE_GATT_CHR_F_WRITE,

        .arg = (void *)(uintptr_t)0xFF01,
    },


    {
        /*
         * FF02
         *
         * Sensor telemetry.
         */

        .uuid = BLE_UUID16_DECLARE(0xFF02),

        .access_cb = gatt_access,

        .flags =
            BLE_GATT_CHR_F_READ |
            BLE_GATT_CHR_F_NOTIFY,

        .val_handle = &s_chr_sensor_val,

        .arg = (void *)(uintptr_t)0xFF02,
    },


    {
        /*
         * FF03
         *
         * Control / command characteristic.
         */

        .uuid = BLE_UUID16_DECLARE(0xFF03),

        .access_cb = gatt_access,

        .flags =
            BLE_GATT_CHR_F_WRITE,

        .arg = (void *)(uintptr_t)0xFF03,
    },


    {
        /*
         * FF04
         *
         * Diagnostics.
         */

        .uuid = BLE_UUID16_DECLARE(0xFF04),

        .access_cb = gatt_access,

        .flags =
            BLE_GATT_CHR_F_READ,

        .arg = (void *)(uintptr_t)0xFF04,
    },


    {
        /*
         * FF05
         *
         * Firmware version.
         */

        .uuid = BLE_UUID16_DECLARE(0xFF05),

        .access_cb = gatt_access,

        .flags =
            BLE_GATT_CHR_F_READ,

        .arg = (void *)(uintptr_t)0xFF05,
    },


    {
        0
    }
};


/* --------------------------------------------------------------------------
 * GATT service
 * -------------------------------------------------------------------------- */

static const struct ble_gatt_svc_def s_svcs[] = {

    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,

        .uuid = &s_svc_uuid.u,

        .characteristics = s_chrs,
    },

    {
        0
    }
};


/* --------------------------------------------------------------------------
 * GATT access callback
 * -------------------------------------------------------------------------- */

static int gatt_access(
    uint16_t conn,
    uint16_t attr,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg
)
{
    (void)conn;
    (void)attr;


    uint16_t uuid16 =
        (uint16_t)(uintptr_t)arg;


    /* ----------------------------------------------------------------------
     * READ
     * ---------------------------------------------------------------------- */

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {

        const char *p = "ok";

        uint16_t n = 2;


        if (uuid16 == 0xFF02) {

            p = (const char *)s_sensor_cache;

            n =
                (s_sensor_len == 0U)
                ? 2U
                : s_sensor_len;
        }


        else if (uuid16 == 0xFF04) {

            p = "diag:ok";

            n = 7;
        }


        else if (uuid16 == 0xFF05) {

            p = IOT_FW_VERSION_STRING;

            n =
                (uint16_t)strlen(
                    IOT_FW_VERSION_STRING
                );
        }


        else if (uuid16 == 0xFF01) {

            p =
                iot_config_get()->provisioned
                ? "prov=1"
                : "prov=0";

            n =
                (uint16_t)strlen(p);
        }


        int rc =
            os_mbuf_append(
                ctxt->om,
                p,
                n
            );


        return
            (rc == 0)
            ? 0
            : BLE_ATT_ERR_INSUFFICIENT_RES;
    }


    /* ----------------------------------------------------------------------
     * WRITE
     * ---------------------------------------------------------------------- */

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {


        uint16_t om_len =
            OS_MBUF_PKTLEN(ctxt->om);


        if (om_len == 0U) {

            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }


        /* ------------------------------------------------------------------
         * FF01 = BLE provisioning
         * ------------------------------------------------------------------ */

        if (uuid16 == 0xFF01) {


            /*
             * Check available buffer space.
             */

            uint16_t available =
                (uint16_t)(
                    sizeof(s_provision_buf) - 1U
                );


            if (
                om_len >
                (uint16_t)(
                    available - s_provision_len
                )
            ) {

                IOT_LOGW(
                    TAG,
                    "BLE provisioning packet too large len=%u accumulated=%u",
                    (unsigned)om_len,
                    (unsigned)s_provision_len
                );


                /*
                 * Clear corrupted/incomplete transaction.
                 */

                s_provision_len = 0U;

                memset(
                    s_provision_buf,
                    0,
                    sizeof(s_provision_buf)
                );


                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }


            /* --------------------------------------------------------------
             * Copy BLE fragment into reassembly buffer
             * -------------------------------------------------------------- */

            int rc =
                ble_hs_mbuf_to_flat(
                    ctxt->om,
                    &s_provision_buf[s_provision_len],
                    om_len,
                    NULL
                );


            if (rc != 0) {

                IOT_LOGW(
                    TAG,
                    "BLE provisioning copy failed rc=%d",
                    rc
                );


                s_provision_len = 0U;


                memset(
                    s_provision_buf,
                    0,
                    sizeof(s_provision_buf)
                );


                return BLE_ATT_ERR_UNLIKELY;
            }


            /*
             * Update accumulated length.
             */

            s_provision_len =
                (uint16_t)(
                    s_provision_len +
                    om_len
                );


            /*
             * Always keep a NULL terminator.
             */

            s_provision_buf[s_provision_len] =
                '\0';


            IOT_LOGI(
                TAG,
                "BLE provisioning fragment len=%u total=%u",
                (unsigned)om_len,
                (unsigned)s_provision_len
            );


            /* --------------------------------------------------------------
             * Validate beginning of JSON
             * -------------------------------------------------------------- */

            if (
                s_provision_len == 0U ||
                s_provision_buf[0] != '{'
            ) {

                IOT_LOGW(
                    TAG,
                    "BLE provisioning invalid JSON start"
                );


                s_provision_len = 0U;


                memset(
                    s_provision_buf,
                    0,
                    sizeof(s_provision_buf)
                );


                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }


            /* --------------------------------------------------------------
             * JSON is incomplete
             * -------------------------------------------------------------- */

            if (
                s_provision_buf[s_provision_len - 1U]
                != '}'
            ) {

                /*
                 * Do NOT send anything to command_q yet.
                 *
                 * Wait for the next BLE fragment.
                 */

                return 0;
            }


            /* --------------------------------------------------------------
             * COMPLETE JSON RECEIVED
             * -------------------------------------------------------------- */

            iot_command_msg_t cmd;

            memset(
                &cmd,
                0,
                sizeof(cmd)
            );


            cmd.opcode = 0xFF01;


            cmd.payload_len =
                s_provision_len;


            memcpy(
                cmd.payload,
                s_provision_buf,
                s_provision_len
            );


            /*
             * Make sure payload is NULL terminated.
             *
             * payload has one extra byte because the maximum JSON
             * length is limited by IOT_JSON_SMALL_LEN.
             */

            if (
                cmd.payload_len <
                sizeof(cmd.payload)
            ) {

                cmd.payload[cmd.payload_len] =
                    '\0';
            }


            IOT_LOGI(
                TAG,
                "BLE provisioning complete len=%u payload=%s",
                (unsigned)cmd.payload_len,
                (char *)cmd.payload
            );


            /*
             * Reset accumulator BEFORE queueing.
             *
             * This makes the next provisioning transaction start cleanly.
             */

            s_provision_len = 0U;


            memset(
                s_provision_buf,
                0,
                sizeof(s_provision_buf)
            );


            /* --------------------------------------------------------------
             * Send ONE complete command to command queue
             * -------------------------------------------------------------- */

            iot_task_ctx_t *ctx =
                iot_tasks_ctx();


            if (
                (ctx == NULL) ||
                (ctx->command_q == NULL)
            ) {

                IOT_LOGW(
                    TAG,
                    "BLE provisioning command queue unavailable"
                );


                return BLE_ATT_ERR_UNLIKELY;
            }


            iot_err_t queue_err =
                iot_queue_send(
                    ctx->command_q,
                    &cmd,
                    0U
                );


            if (IOT_FAIL(queue_err)) {

                IOT_LOGW(
                    TAG,
                    "BLE provisioning command queue full err=%d",
                    (int)queue_err
                );


                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }


            return 0;
        }


        /* ------------------------------------------------------------------
         * NORMAL BLE COMMANDS
         *
         * FF02 = sensor
         * FF03 = control
         * FF04 = diagnostics
         * FF05 = firmware information
         * ------------------------------------------------------------------ */


        iot_command_msg_t cmd;

        memset(
            &cmd,
            0,
            sizeof(cmd)
        );


        cmd.opcode =
            uuid16;


        if (
            om_len >
            sizeof(cmd.payload)
        ) {

            om_len =
                (uint16_t)sizeof(cmd.payload);
        }


        cmd.payload_len =
            om_len;


        (void)ble_hs_mbuf_to_flat(
            ctxt->om,
            cmd.payload,
            om_len,
            NULL
        );


        IOT_LOGI(
            TAG,
            "BLE WRITE opcode=0x%04x len=%u payload=%.*s",
            (unsigned)cmd.opcode,
            (unsigned)cmd.payload_len,
            (int)cmd.payload_len,
            (char *)cmd.payload
        );


        iot_task_ctx_t *ctx =
            iot_tasks_ctx();


        if (
            (ctx != NULL) &&
            (ctx->command_q != NULL)
        ) {

            (void)iot_queue_send(
                ctx->command_q,
                &cmd,
                0U
            );
        }


        return 0;
    }


    return BLE_ATT_ERR_UNLIKELY;
}


/* --------------------------------------------------------------------------
 * BLE synchronization
 * -------------------------------------------------------------------------- */

static void ble_on_sync(void)
{
    (void)ble_hs_util_ensure_addr(0);

    (void)iot_ble_start_advertising();
}


/* --------------------------------------------------------------------------
 * GAP event handler
 * -------------------------------------------------------------------------- */

static int gap_event(
    struct ble_gap_event *ev,
    void *arg
)
{
    (void)arg;


    switch (ev->type) {


    case BLE_GAP_EVENT_CONNECT:

        if (ev->connect.status == 0) {

            s_conn =
                ev->connect.conn_handle;

            s_connected = true;

            s_state =
                IOT_BLE_CONNECTED;


            iot_task_ctx_t *ctx =
                iot_tasks_ctx();


            if (
                (ctx != NULL) &&
                (ctx->system_evt != NULL)
            ) {

                iot_event_set(
                    ctx->system_evt,
                    IOT_EVT_BLE_CONN
                );
            }

        }

        else {

            s_state =
                IOT_BLE_ADVERTISING;

            (void)iot_ble_start_advertising();
        }

        break;


    case BLE_GAP_EVENT_DISCONNECT:

        s_connected = false;

        s_state =
            IOT_BLE_ADVERTISING;

        (void)iot_ble_start_advertising();

        break;


    default:

        break;
    }


    return 0;
}


/* --------------------------------------------------------------------------
 * NimBLE host task
 * -------------------------------------------------------------------------- */

static void nimble_host_task(void *param)
{
    (void)param;


    nimble_port_run();


    nimble_port_freertos_deinit();
}


#endif /* ESP_PLATFORM */


/* --------------------------------------------------------------------------
 * BLE initialization
 * -------------------------------------------------------------------------- */

iot_err_t iot_ble_init(
    const char *device_name
)
{
    if (device_name != NULL) {

        (void)strncpy(
            s_name,
            device_name,
            sizeof(s_name) - 1U
        );

        s_name[
            sizeof(s_name) - 1U
        ] = '\0';
    }


#ifdef ESP_PLATFORM

    esp_err_t err =
        nimble_port_init();


    if (err != ESP_OK) {

        return iot_err_from_esp(err);
    }


    /*
     * BLE security configuration.
     */

    ble_hs_cfg.sync_cb =
        ble_on_sync;


    ble_hs_cfg.sm_bonding = 1;

    ble_hs_cfg.sm_sc = 1;

    ble_hs_cfg.sm_mitm = 1;

    ble_hs_cfg.sm_io_cap =
        BLE_HS_IO_NO_INPUT_OUTPUT;


    /*
     * GAP / GATT services.
     */

    ble_svc_gap_init();

    ble_svc_gatt_init();


    /*
     * Device name.
     */

    (void)ble_svc_gap_device_name_set(
        s_name
    );


    /*
     * Register GATT service.
     */

    (void)ble_gatts_count_cfg(
        s_svcs
    );


    (void)ble_gatts_add_svcs(
        s_svcs
    );


    /*
     * Start NimBLE host task.
     */

    nimble_port_freertos_init(
        nimble_host_task
    );


    s_state =
        IOT_BLE_ADVERTISING;


#else

    s_state =
        IOT_BLE_ADVERTISING;

#endif


    IOT_LOGI(
        TAG,
        "ble init name=%s",
        s_name
    );


    return IOT_OK;
}


/* --------------------------------------------------------------------------
 * Start BLE advertising
 * -------------------------------------------------------------------------- */

iot_err_t iot_ble_start_advertising(void)
{
#ifdef ESP_PLATFORM

    struct ble_gap_adv_params ap;

    struct ble_hs_adv_fields fields;


    memset(
        &fields,
        0,
        sizeof(fields)
    );


    /*
     * General discoverable + BLE only.
     */

    fields.flags =
        BLE_HS_ADV_F_DISC_GEN |
        BLE_HS_ADV_F_BREDR_UNSUP;


    /*
     * Advertise device name.
     */

    fields.name =
        (uint8_t *)s_name;


    fields.name_len =
        (uint8_t)strlen(s_name);


    fields.name_is_complete =
        1;


    (void)ble_gap_adv_set_fields(
        &fields
    );


    memset(
        &ap,
        0,
        sizeof(ap)
    );


    ap.conn_mode =
        BLE_GAP_CONN_MODE_UND;


    ap.disc_mode =
        BLE_GAP_DISC_MODE_GEN;


    int rc =
        ble_gap_adv_start(
            BLE_OWN_ADDR_PUBLIC,
            NULL,
            BLE_HS_FOREVER,
            &ap,
            gap_event,
            NULL
        );


    if (
        rc != 0 &&
        rc != BLE_HS_EALREADY
    ) {

        IOT_LOGW(
            TAG,
            "adv start rc=%d",
            rc
        );


        return IOT_ERR_FAIL;
    }

#endif


    s_state =
        IOT_BLE_ADVERTISING;


    return IOT_OK;
}


/* --------------------------------------------------------------------------
 * Stop BLE advertising
 * -------------------------------------------------------------------------- */

iot_err_t iot_ble_stop_advertising(void)
{
#ifdef ESP_PLATFORM

    (void)ble_gap_adv_stop();

#endif


    s_state =
        IOT_BLE_IDLE;


    return IOT_OK;
}


/* --------------------------------------------------------------------------
 * BLE state
 * -------------------------------------------------------------------------- */

iot_ble_state_t iot_ble_state(void)
{
    return s_state;
}


/* --------------------------------------------------------------------------
 * BLE connection state
 * -------------------------------------------------------------------------- */

bool iot_ble_is_connected(void)
{
    return s_connected;
}


/* --------------------------------------------------------------------------
 * BLE sensor notification
 * -------------------------------------------------------------------------- */

iot_err_t iot_ble_notify_sensor(
    const uint8_t *data,
    uint16_t len
)
{
    if (
        (data == NULL) ||
        (len == 0U)
    ) {

        return IOT_ERR_INVALID_ARG;
    }


    if (
        len >
        sizeof(s_sensor_cache)
    ) {

        len =
            (uint16_t)sizeof(s_sensor_cache);
    }


#ifdef ESP_PLATFORM

    memcpy(
        s_sensor_cache,
        data,
        len
    );


    s_sensor_len =
        len;


    /*
     * No connected BLE client.
     *
     * Keep the sensor data cached.
     */

    if (!s_connected) {

        return IOT_OK;
    }


    struct os_mbuf *om =
        ble_hs_mbuf_from_flat(
            data,
            len
        );


    if (om == NULL) {

        return IOT_ERR_NO_MEM;
    }


    int rc =
        ble_gatts_notify_custom(
            s_conn,
            s_chr_sensor_val,
            om
        );


    return
        (rc == 0)
        ? IOT_OK
        : IOT_ERR_FAIL;


#else

    (void)data;

    (void)len;

    return IOT_OK;

#endif
}
