/**
 * @file iot_wifi.c
 * @copyright Copyright (c) 2026 Embedded AI Design Labs Pvt Ltd.
 * @author    Muhammad Samiullah, CTO & Founder
 * @brief STA connection manager with detailed Wi-Fi diagnostics.
 *
 * @thread wifi_mgr task + Wi-Fi event task (IDF)
 * @syscalls esp_wifi_*, esp_netif_*, esp_event_*
 */

#include "iot_wifi.h"
#include "iot_log.h"
#include "iot_rtos.h"
#include "iot_tasks.h"

#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static const char *TAG = "iot_wifi";

static iot_wifi_state_t s_state = WIFI_DISCONNECTED;
static iot_mutex_t s_lock;
static iot_wifi_status_t s_status;

static uint32_t s_backoff_ms = 1000U;

static char s_ssid[33];
static char s_pass[65];


/* --------------------------------------------------------------------------
 * Wi-Fi state helper
 * -------------------------------------------------------------------------- */

static void set_state(iot_wifi_state_t st)
{
    (void)iot_mutex_lock(s_lock, 1000U);

    s_state = st;
    s_status.state = st;

    iot_mutex_unlock(s_lock);

    /*
     * Notify the rest of the platform about Wi-Fi state.
     *
     * main.c waits for IOT_EVT_WIFI_IP before starting MQTT.
     */
    iot_task_ctx_t *ctx = iot_tasks_ctx();

    if ((ctx != NULL) &&
        (ctx->system_evt != NULL)) {

        if (st == WIFI_IP_READY) {

            iot_event_set(
                ctx->system_evt,
                IOT_EVT_WIFI_IP
            );

            IOT_LOGI(
                TAG,
                "IOT_EVT_WIFI_IP set"
            );

        } else {

            iot_event_clear(
                ctx->system_evt,
                IOT_EVT_WIFI_IP | IOT_EVT_MQTT_UP
            );
        }
    }
}


/* --------------------------------------------------------------------------
 * Wi-Fi event handler
 * -------------------------------------------------------------------------- */

#ifdef ESP_PLATFORM

static void wifi_event(
    void *arg,
    esp_event_base_t base,
    int32_t id,
    void *data)
{
    (void)arg;


    /* ----------------------------------------------------------------------
     * Wi-Fi events
     * ---------------------------------------------------------------------- */

    if (base == WIFI_EVENT) {

        /* ------------------------------------------------------------------
         * STA started
         * ------------------------------------------------------------------ */

        if (id == WIFI_EVENT_STA_START) {

            IOT_LOGI(
                TAG,
                "WIFI_EVENT_STA_START"
            );

            set_state(
                WIFI_CONNECTING
            );

            esp_err_t err =
                esp_wifi_connect();

            if (err != ESP_OK) {

                IOT_LOGE(
                    TAG,
                    "esp_wifi_connect failed err=0x%x",
                    (unsigned)err
                );
            }

            return;
        }


        /* ------------------------------------------------------------------
         * STA connected to AP
         * ------------------------------------------------------------------ */

        if (id == WIFI_EVENT_STA_CONNECTED) {

            wifi_event_sta_connected_t *ev =
                (wifi_event_sta_connected_t *)data;

            set_state(
                WIFI_CONNECTED
            );

            s_backoff_ms = 1000U;

            if (ev != NULL) {

                IOT_LOGI(
                    TAG,
                    "========================================"
                );

                IOT_LOGI(
                    TAG,
                    "Wi-Fi connected to AP"
                );

                IOT_LOGI(
                    TAG,
                    "SSID    : %s",
                    ev->ssid
                );

                IOT_LOGI(
                    TAG,
                    "Channel : %d",
                    ev->channel
                );

                IOT_LOGI(
                    TAG,
                    "========================================"
                );

            } else {

                IOT_LOGI(
                    TAG,
                    "Wi-Fi connected to AP"
                );
            }

            return;
        }


        /* ------------------------------------------------------------------
         * STA disconnected
         * ------------------------------------------------------------------ */

        if (id == WIFI_EVENT_STA_DISCONNECTED) {

            wifi_event_sta_disconnected_t *ev =
                (wifi_event_sta_disconnected_t *)data;

            int reason = -1;

            if (ev != NULL) {
                reason = (int)ev->reason;
            }

            s_status.retry_count++;

            IOT_LOGW(
                TAG,
                "Wi-Fi disconnected: reason=%d retry=%lu",
                reason,
                (unsigned long)s_status.retry_count
            );


            /* --------------------------------------------------------------
             * Diagnostic reason
             * -------------------------------------------------------------- */

            switch (reason) {

                case WIFI_REASON_AUTH_EXPIRE:

                    IOT_LOGW(
                        TAG,
                        "reason: authentication expired"
                    );

                    break;


                case WIFI_REASON_AUTH_LEAVE:

                    IOT_LOGW(
                        TAG,
                        "reason: authentication left"
                    );

                    break;


                case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:

                    IOT_LOGW(
                        TAG,
                        "reason: WPA 4-way handshake timeout"
                    );

                    break;


                case WIFI_REASON_BEACON_TIMEOUT:

                    IOT_LOGW(
                        TAG,
                        "reason: beacon timeout"
                    );

                    break;


                case WIFI_REASON_NO_AP_FOUND:

                    IOT_LOGW(
                        TAG,
                        "reason: AP not found"
                    );

                    break;


                case WIFI_REASON_AUTH_FAIL:

                    IOT_LOGW(
                        TAG,
                        "reason: authentication failed"
                    );

                    break;


                case WIFI_REASON_ASSOC_FAIL:

                    IOT_LOGW(
                        TAG,
                        "reason: association failed"
                    );

                    break;


                case WIFI_REASON_HANDSHAKE_TIMEOUT:

                    IOT_LOGW(
                        TAG,
                        "reason: handshake timeout"
                    );

                    break;


                case WIFI_REASON_CONNECTION_FAIL:

                    IOT_LOGW(
                        TAG,
                        "reason: connection failed"
                    );

                    break;


                default:

                    IOT_LOGW(
                        TAG,
                        "reason: unknown/unhandled reason code"
                    );

                    break;
            }


            set_state(
                WIFI_RECONNECTING
            );


            /* --------------------------------------------------------------
             * Exponential backoff
             * -------------------------------------------------------------- */

            uint32_t delay =
                s_backoff_ms;

            if (s_backoff_ms < 30000U) {

                s_backoff_ms *= 2U;

                if (s_backoff_ms > 30000U) {
                    s_backoff_ms = 30000U;
                }
            }


            /*
             * Do not block the event handler for a long time.
             * Limit reconnect delay to 50 ms.
             */

            if (delay > 50U) {
                delay = 50U;
            }


            vTaskDelay(
                pdMS_TO_TICKS(delay)
            );


            esp_err_t err =
                esp_wifi_connect();

            if (err != ESP_OK) {

                IOT_LOGW(
                    TAG,
                    "reconnect request failed err=0x%x",
                    (unsigned)err
                );

            } else {

                IOT_LOGI(
                    TAG,
                    "Wi-Fi reconnect requested"
                );
            }


            set_state(
                WIFI_CONNECTING
            );

            return;
        }
    }


    /* ----------------------------------------------------------------------
     * IP events
     * ---------------------------------------------------------------------- */

    if ((base == IP_EVENT) &&
        (id == IP_EVENT_STA_GOT_IP)) {

        ip_event_got_ip_t *ev =
            (ip_event_got_ip_t *)data;


        if (ev != NULL) {

            /* --------------------------------------------------------------
             * Store IP address
             * -------------------------------------------------------------- */

            (void)snprintf(
                s_status.ip,
                sizeof(s_status.ip),
                IPSTR,
                IP2STR(&ev->ip_info.ip)
            );


            /* --------------------------------------------------------------
             * Display network information
             * -------------------------------------------------------------- */

            IOT_LOGI(
                TAG,
                "========================================"
            );

            IOT_LOGI(
                TAG,
                "Wi-Fi GOT IP"
            );

            IOT_LOGI(
                TAG,
                "IP address : %s",
                s_status.ip
            );

            IOT_LOGI(
                TAG,
                "netmask    : " IPSTR,
                IP2STR(&ev->ip_info.netmask)
            );

            IOT_LOGI(
                TAG,
                "gateway    : " IPSTR,
                IP2STR(&ev->ip_info.gw)
            );

            IOT_LOGI(
                TAG,
                "========================================"
            );
        }


        /*
         * IMPORTANT:
         *
         * set_state(WIFI_IP_READY) sets IOT_EVT_WIFI_IP.
         *
         * main.c waits for this event before starting MQTT.
         */

        set_state(
            WIFI_IP_READY
        );

        return;
    }
}

#endif /* ESP_PLATFORM */


/* --------------------------------------------------------------------------
 * Wi-Fi initialization
 * -------------------------------------------------------------------------- */

iot_err_t iot_wifi_init(void)
{
    iot_err_t e =
        iot_mutex_create(
            &s_lock
        );

    if (IOT_FAIL(e)) {
        return e;
    }


    (void)memset(
        &s_status,
        0,
        sizeof(s_status)
    );


#ifdef ESP_PLATFORM

    /* ----------------------------------------------------------------------
     * Initialize TCP/IP network interface
     * ---------------------------------------------------------------------- */

    ESP_ERROR_CHECK(
        esp_netif_init()
    );


    /* ----------------------------------------------------------------------
     * Create default event loop
     * ---------------------------------------------------------------------- */

    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );


    /* ----------------------------------------------------------------------
     * Create default Wi-Fi STA interface
     * ---------------------------------------------------------------------- */

    (void)esp_netif_create_default_wifi_sta();


    /* ----------------------------------------------------------------------
     * Initialize Wi-Fi driver
     * ---------------------------------------------------------------------- */

    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    esp_err_t err =
        esp_wifi_init(
            &cfg
        );

    if (err != ESP_OK) {

        return iot_err_from_esp(
            err
        );
    }


    /* ----------------------------------------------------------------------
     * Register Wi-Fi events
     * ---------------------------------------------------------------------- */

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event,
            NULL
        )
    );


    /* ----------------------------------------------------------------------
     * Register IP event
     * ---------------------------------------------------------------------- */

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_event,
            NULL
        )
    );


    /* ----------------------------------------------------------------------
     * Station mode
     * ---------------------------------------------------------------------- */

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            WIFI_MODE_STA
        )
    );

#endif


    s_state =
        WIFI_DISCONNECTED;


    IOT_LOGI(
        TAG,
        "wifi mgr init"
    );


    return IOT_OK;
}


/* --------------------------------------------------------------------------
 * Start Wi-Fi
 * -------------------------------------------------------------------------- */

iot_err_t iot_wifi_start(
    const char *ssid,
    const char *pass)
{
    if ((ssid == NULL) ||
        (ssid[0] == '\0')) {

        return IOT_ERR_INVALID_ARG;
    }


    /* ----------------------------------------------------------------------
     * Clear previous credentials
     * ---------------------------------------------------------------------- */

    (void)memset(
        s_ssid,
        0,
        sizeof(s_ssid)
    );

    (void)memset(
        s_pass,
        0,
        sizeof(s_pass)
    );


    /* ----------------------------------------------------------------------
     * Copy SSID
     * ---------------------------------------------------------------------- */

    (void)strncpy(
        s_ssid,
        ssid,
        sizeof(s_ssid) - 1U
    );


    /* ----------------------------------------------------------------------
     * Copy password
     * ---------------------------------------------------------------------- */

    if (pass != NULL) {

        (void)strncpy(
            s_pass,
            pass,
            sizeof(s_pass) - 1U
        );
    }


    IOT_LOGI(
        TAG,
        "Wi-Fi start requested ssid=%s password_len=%u",
        s_ssid,
        (unsigned)strlen(s_pass)
    );


#ifdef ESP_PLATFORM

    wifi_config_t wc =
        {0};


    /* ----------------------------------------------------------------------
     * Configure SSID
     * ---------------------------------------------------------------------- */

    (void)strncpy(
        (char *)wc.sta.ssid,
        s_ssid,
        sizeof(wc.sta.ssid) - 1U
    );


    /* ----------------------------------------------------------------------
     * Configure password
     * ---------------------------------------------------------------------- */

    (void)strncpy(
        (char *)wc.sta.password,
        s_pass,
        sizeof(wc.sta.password) - 1U
    );


    /*
     * WPA2/WPA3 compatible configuration.
     */

    wc.sta.threshold.authmode =
        WIFI_AUTH_WPA2_PSK;

    wc.sta.sae_pwe_h2e =
        WPA3_SAE_PWE_BOTH;


    /* ----------------------------------------------------------------------
     * Apply configuration
     * ---------------------------------------------------------------------- */

    esp_err_t err =
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wc
        );

    if (err != ESP_OK) {

        IOT_LOGE(
            TAG,
            "esp_wifi_set_config failed err=0x%x",
            (unsigned)err
        );

        set_state(
            WIFI_ERROR
        );

        return iot_err_from_esp(
            err
        );
    }


    IOT_LOGI(
        TAG,
        "Wi-Fi configuration accepted"
    );


    set_state(
        WIFI_CONNECTING
    );


    /* ----------------------------------------------------------------------
     * Start Wi-Fi
     * ---------------------------------------------------------------------- */

    err =
        esp_wifi_start();


    if (err == ESP_ERR_WIFI_STATE) {

        /*
         * Wi-Fi is already started.
         * Try connecting using the newly configured credentials.
         */

        IOT_LOGI(
            TAG,
            "Wi-Fi already started, requesting connect"
        );

        err =
            esp_wifi_connect();
    }


    if (err != ESP_OK) {

        IOT_LOGE(
            TAG,
            "Wi-Fi start/connect failed err=0x%x",
            (unsigned)err
        );

        set_state(
            WIFI_ERROR
        );

        return iot_err_from_esp(
            err
        );
    }


    IOT_LOGI(
        TAG,
        "Wi-Fi start/connect request accepted"
    );


    return IOT_OK;


#else

    /* ----------------------------------------------------------------------
     * Host simulation
     * ---------------------------------------------------------------------- */

    set_state(
        WIFI_IP_READY
    );

    (void)memcpy(
        s_status.ip,
        "127.0.0.1",
        10
    );

    IOT_LOGI(
        TAG,
        "sim wifi ip-ready ssid=%s",
        s_ssid
    );

    return IOT_OK;

#endif
}


/* --------------------------------------------------------------------------
 * Stop Wi-Fi
 * -------------------------------------------------------------------------- */

iot_err_t iot_wifi_stop(void)
{
#ifdef ESP_PLATFORM

    (void)esp_wifi_disconnect();

    (void)esp_wifi_stop();

#endif


    set_state(
        WIFI_DISCONNECTED
    );


    return IOT_OK;
}


/* --------------------------------------------------------------------------
 * Current Wi-Fi state
 * -------------------------------------------------------------------------- */

iot_wifi_state_t iot_wifi_state(void)
{
    return s_state;
}


/* --------------------------------------------------------------------------
 * Wi-Fi status
 * -------------------------------------------------------------------------- */

iot_err_t iot_wifi_get_status(
    iot_wifi_status_t *out)
{
    if (out == NULL) {

        return IOT_ERR_INVALID_ARG;
    }


#ifdef ESP_PLATFORM

    wifi_ap_record_t ap;

    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {

        s_status.rssi =
            ap.rssi;
    }

#endif


    (void)iot_mutex_lock(
        s_lock,
        100U
    );


    *out =
        s_status;


    iot_mutex_unlock(
        s_lock
    );


    return IOT_OK;
}