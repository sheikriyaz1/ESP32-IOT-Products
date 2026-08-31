/**
 * @file iot_provision.c
 * @copyright Copyright (c) 2026 Embedded AI Design Labs Pvt Ltd.
 * @author    Muhammad Samiullah, CTO & Founder
 * @brief Minimal JSON field extractor for BLE commissioning payloads.
 *
 * Expected keys:
 *   ssid
 *   pass
 *   mqtt
 *   user
 *   mpw
 *   cloud
 *
 * BLE provisioning flow:
 *
 *   BLE JSON
 *      |
 *      v
 *   Validate SSID/password
 *      |
 *      v
 *   Save configuration to NVS
 *      |
 *      v
 *   Start Wi-Fi
 *      |
 *      v
 *   Wait for IOT_EVT_WIFI_IP
 *      |
 *      v
 *   Print IP address
 *      |
 *      v
 *   Start MQTT
 */

#include "iot_provision.h"
#include "iot_log.h"
#include "iot_security.h"
#include "iot_tasks.h"
#include "iot_wifi.h"
#include "iot_mqtt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "iot_prov";


/* --------------------------------------------------------------------------
 * JSON string extractor
 * -------------------------------------------------------------------------- */

static bool extract_str(
    const char *json,
    const char *key,
    char *out,
    size_t out_len)
{
    if ((json == NULL) ||
        (key == NULL) ||
        (out == NULL) ||
        (out_len == 0U)) {

        return false;
    }

    char pat[32];

    (void)snprintf(
        pat,
        sizeof(pat),
        "\"%s\"",
        key
    );

    const char *p =
        strstr(json, pat);

    if (p == NULL) {
        return false;
    }

    p =
        strchr(
            p + strlen(pat),
            ':'
        );

    if (p == NULL) {
        return false;
    }

    p++;

    /* Skip spaces */
    while (*p == ' ') {
        p++;
    }

    /* Value must start with quote */
    if (*p != '"') {
        return false;
    }

    p++;

    size_t i = 0U;

    while (
        (p[i] != '\0') &&
        (p[i] != '"') &&
        (i + 1U < out_len)
    ) {
        out[i] = p[i];
        i++;
    }

    out[i] = '\0';

    return (i > 0U);
}


/* --------------------------------------------------------------------------
 * Provisioning initialization
 * -------------------------------------------------------------------------- */

iot_err_t iot_provision_init(void)
{
    return IOT_OK;
}


/* --------------------------------------------------------------------------
 * Check provisioning status
 * -------------------------------------------------------------------------- */

bool iot_provision_is_complete(void)
{
    const iot_runtime_cfg_t *c =
        iot_config_get();

    return
        (c != NULL) &&
        c->provisioned &&
        (c->wifi_ssid[0] != '\0');
}


/* --------------------------------------------------------------------------
 * Apply BLE provisioning JSON
 * -------------------------------------------------------------------------- */

iot_err_t iot_provision_apply_json(
    const char *json,
    size_t len)
{
    /* ----------------------------------------------------------------------
     * Validate input
     * ---------------------------------------------------------------------- */

    if (
        (json == NULL) ||
        (len == 0U) ||
        (len > 512U)
    ) {
        IOT_LOGW(
            TAG,
            "invalid provisioning JSON"
        );

        return IOT_ERR_INVALID_ARG;
    }


    /* ----------------------------------------------------------------------
     * Load existing configuration
     * ---------------------------------------------------------------------- */

    iot_runtime_cfg_t cfg;

    iot_err_t load_err =
        iot_config_load(&cfg);

    if (IOT_FAIL(load_err)) {

        IOT_LOGW(
            TAG,
            "config load failed err=%d",
            (int)load_err
        );

        return load_err;
    }


    /* ----------------------------------------------------------------------
     * Extract Wi-Fi SSID
     * ---------------------------------------------------------------------- */

    bool ssid_ok =
        extract_str(
            json,
            "ssid",
            cfg.wifi_ssid,
            sizeof(cfg.wifi_ssid)
        );


    /* ----------------------------------------------------------------------
     * Extract Wi-Fi password
     * ---------------------------------------------------------------------- */

    bool pass_ok =
        extract_str(
            json,
            "pass",
            cfg.wifi_pass,
            sizeof(cfg.wifi_pass)
        );


    /* ----------------------------------------------------------------------
     * Print provisioning result
     * ---------------------------------------------------------------------- */

    IOT_LOGI(
        TAG,
        "provision JSON: ssid_ok=%d pass_ok=%d pass_len=%u",
        (int)ssid_ok,
        (int)pass_ok,
        (unsigned)strlen(cfg.wifi_pass)
    );


    /* ----------------------------------------------------------------------
     * Validate Wi-Fi credentials
     * ---------------------------------------------------------------------- */

    if (!ssid_ok) {

        IOT_LOGW(
            TAG,
            "Wi-Fi SSID missing"
        );

        return IOT_ERR_INVALID_ARG;
    }

    if (!pass_ok) {

        IOT_LOGW(
            TAG,
            "Wi-Fi password missing"
        );

        return IOT_ERR_INVALID_ARG;
    }


    /* ----------------------------------------------------------------------
     * Extract MQTT configuration
     * ---------------------------------------------------------------------- */

    (void)extract_str(
        json,
        "mqtt",
        cfg.mqtt_uri,
        sizeof(cfg.mqtt_uri)
    );


    (void)extract_str(
        json,
        "user",
        cfg.mqtt_user,
        sizeof(cfg.mqtt_user)
    );


    (void)extract_str(
        json,
        "mpw",
        cfg.mqtt_pass,
        sizeof(cfg.mqtt_pass)
    );


    /* ----------------------------------------------------------------------
     * Create device ID if necessary
     * ---------------------------------------------------------------------- */

    if (
        cfg.device_id[0] == '\0' ||
        strstr(
            cfg.device_id,
            "unprovisioned"
        ) != NULL
    ) {

        (void)iot_security_default_device_id(
            cfg.device_id,
            sizeof(cfg.device_id)
        );

        IOT_LOGI(
            TAG,
            "device ID generated: %s",
            cfg.device_id
        );
    }


    /* ----------------------------------------------------------------------
     * Mark device as provisioned
     * ---------------------------------------------------------------------- */

    cfg.provisioned =
        (cfg.wifi_ssid[0] != '\0');


    /* ----------------------------------------------------------------------
     * Save configuration to NVS
     * ---------------------------------------------------------------------- */

    iot_err_t save_err =
        iot_config_save(&cfg);

    if (IOT_FAIL(save_err)) {

        IOT_LOGW(
            TAG,
            "configuration save failed err=%d",
            (int)save_err
        );

        return save_err;
    }


    IOT_LOGI(
        TAG,
        "config saved provisioned=%d",
        (int)cfg.provisioned
    );


    /* ----------------------------------------------------------------------
     * Set provisioned event
     * ---------------------------------------------------------------------- */

    if (cfg.provisioned) {

        iot_task_ctx_t *ctx =
            iot_tasks_ctx();

        if (
            (ctx != NULL) &&
            (ctx->system_evt != NULL)
        ) {

            iot_event_set(
                ctx->system_evt,
                IOT_EVT_PROVISIONED
            );
        }
    }


    IOT_LOGI(
        TAG,
        "provisioned ssid=%s",
        cfg.wifi_ssid
    );


    /* ----------------------------------------------------------------------
     * Start Wi-Fi immediately after BLE provisioning
     * ---------------------------------------------------------------------- */

    IOT_LOGI(
        TAG,
        "starting Wi-Fi after BLE provisioning ssid=%s",
        cfg.wifi_ssid
    );


    iot_err_t wifi_err =
        iot_wifi_start(
            cfg.wifi_ssid,
            cfg.wifi_pass
        );


    if (IOT_FAIL(wifi_err)) {

        IOT_LOGW(
            TAG,
            "Wi-Fi start failed err=%d",
            (int)wifi_err
        );

        return wifi_err;
    }


    IOT_LOGI(
        TAG,
        "Wi-Fi start/connect request accepted"
    );


    /* ----------------------------------------------------------------------
     * Wait for Wi-Fi to obtain an IP address
     *
     * IMPORTANT:
     *
     * iot_wifi.c sets IOT_EVT_WIFI_IP when:
     *
     *     IP_EVENT_STA_GOT_IP
     *
     * occurs.
     * ---------------------------------------------------------------------- */

    iot_task_ctx_t *ctx =
        iot_tasks_ctx();


    if (
        (ctx == NULL) ||
        (ctx->system_evt == NULL)
    ) {

        IOT_LOGW(
            TAG,
            "system event group unavailable"
        );

        return IOT_ERR_INVALID_STATE;
    }


    IOT_LOGI(
        TAG,
        "waiting for Wi-Fi IP address..."
    );


    uint32_t bits =
        iot_event_wait(
            ctx->system_evt,
            IOT_EVT_WIFI_IP,
            false,
            true,
            30000U
        );


    /* ----------------------------------------------------------------------
     * Check whether Wi-Fi obtained IP
     * ---------------------------------------------------------------------- */

    if (
        (bits & IOT_EVT_WIFI_IP) == 0U
    ) {

        IOT_LOGW(
            TAG,
            "Wi-Fi timeout - no IP address received"
        );

        return IOT_ERR_FAIL;
    }


    /* ----------------------------------------------------------------------
     * Get Wi-Fi status
     * ---------------------------------------------------------------------- */

    iot_wifi_status_t status;

    (void)memset(
        &status,
        0,
        sizeof(status)
    );


    iot_err_t status_err =
        iot_wifi_get_status(&status);


    if (IOT_SUCCESS(status_err)) {

        IOT_LOGI(
            TAG,
            "========================================"
        );

        IOT_LOGI(
            TAG,
            "Wi-Fi connected successfully"
        );

        IOT_LOGI(
            TAG,
            "SSID       : %s",
            cfg.wifi_ssid
        );

        IOT_LOGI(
            TAG,
            "IP address : %s",
            status.ip
        );

        IOT_LOGI(
            TAG,
            "RSSI       : %d dBm",
            (int)status.rssi
        );

        IOT_LOGI(
            TAG,
            "========================================"
        );

    } else {

        IOT_LOGW(
            TAG,
            "Wi-Fi connected but status unavailable"
        );
    }


    /* ----------------------------------------------------------------------
     * Start MQTT after Wi-Fi IP is ready
     * ---------------------------------------------------------------------- */

    IOT_LOGI(
        TAG,
        "wifi IP ready - starting MQTT"
    );


    /*
     * Do not start MQTT twice.
     *
     * If MQTT is already running, simply leave it running.
     */
    iot_mqtt_state_t mqtt_state =
        iot_mqtt_state();


    if (mqtt_state == IOT_MQTT_DOWN) {

        iot_err_t mqtt_err =
            iot_mqtt_start(&cfg);


        if (IOT_FAIL(mqtt_err)) {

            IOT_LOGW(
                TAG,
                "MQTT start failed err=%d",
                (int)mqtt_err
            );

        } else {

            IOT_LOGI(
                TAG,
                "MQTT start requested"
            );
        }

    } else {

        IOT_LOGI(
            TAG,
            "MQTT already running state=%d",
            (int)mqtt_state
        );
    }


    /* ----------------------------------------------------------------------
     * Provisioning complete
     * ---------------------------------------------------------------------- */

    IOT_LOGI(
        TAG,
        "BLE provisioning complete"
    );


    return IOT_OK;
}
