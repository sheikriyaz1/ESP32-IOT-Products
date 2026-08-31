/**
 * @file main.c
 * @copyright Copyright (c) 2026 Embedded AI Design Labs Pvt Ltd.
 * @author    Muhammad Samiullah, CTO & Founder
 * @brief ESP32-C3 IoT platform startup — layered bring-up then profile.
 *
 * Boot sequence:
 *   1. NVS + logging
 *   2. Board + drivers
 *   3. Config + security identity
 *   4. Diagnostics + services + OTA
 *   5. Profile initialization
 *   6. Profile hooks
 *   7. FreeRTOS queues/tasks
 *   8. BLE (always, for commissioning)
 *   9. Wi-Fi if provisioned
 *  10. MQTT after WIFI_IP_READY
 *  11. Mark OTA image valid
 *
 * @syscalls nvs_flash_init, esp_ota_mark_app_valid_cancel_rollback
 * @thread app_main (ESP-IDF main task)
 */

#include "iot_core.h"
#include "iot_board.h"
#include "iot_drivers.h"
#include "iot_rtos.h"
#include "iot_tasks.h"
#include "iot_hooks.h"
#include "iot_wifi.h"
#include "iot_ble.h"
#include "iot_mqtt.h"
#include "iot_security.h"
#include "iot_ota.h"
#include "iot_diag.h"
#include "iot_provision.h"
#include "iot_services.h"
#include "iot_profile.h"
#include "iot_uc.h"
#include "iot_wdt.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs_flash.h"
#include "esp_event.h"
#endif

static const char *TAG = "app_main";


/* --------------------------------------------------------------------------
 * NVS bootstrap
 * -------------------------------------------------------------------------- */

static iot_err_t nvs_bootstrap(void)
{
#ifdef ESP_PLATFORM

    esp_err_t e = nvs_flash_init();

    if (e == ESP_ERR_NVS_NO_FREE_PAGES ||
        e == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());

        e = nvs_flash_init();
    }

    return iot_err_from_esp(e);

#else

    return IOT_OK;

#endif
}


/* --------------------------------------------------------------------------
 * Application entry point
 * -------------------------------------------------------------------------- */

void app_main(void)
{
    /* ----------------------------------------------------------------------
     * 1. Startup banner
     * ---------------------------------------------------------------------- */

    IOT_LOGI(TAG, "platform %s", IOT_PLATFORM_NAME);

    IOT_LOGI(TAG,
             "%s — %s",
             IOT_COPYRIGHT_BRAND,
             IOT_COPYRIGHT_SLOGAN);

    IOT_LOGI(TAG,
             "%s",
             IOT_COPYRIGHT_ORG);

    IOT_LOGI(TAG,
             "%s — %s",
             IOT_COPYRIGHT_AUTHOR,
             IOT_COPYRIGHT_TITLE);

    IOT_LOGI(TAG,
             "© %s Copyright. All rights reserved.",
             IOT_COPYRIGHT_YEAR);


    /* ----------------------------------------------------------------------
     * 2. NVS
     * ---------------------------------------------------------------------- */

    if (nvs_bootstrap() != IOT_OK) {

        IOT_LOGE(TAG, "nvs fail");

        return;
    }


    /* ----------------------------------------------------------------------
     * 3. Core / configuration / board / drivers
     * ---------------------------------------------------------------------- */

    (void)iot_rtos_init();

    (void)iot_config_init();

    (void)iot_board_init();

    (void)iot_drivers_init();


    /* ----------------------------------------------------------------------
     * 4. Security / diagnostics / provisioning / services / OTA
     * ---------------------------------------------------------------------- */

    (void)iot_security_init();

    (void)iot_diag_init();

    (void)iot_provision_init();

    (void)iot_services_init();

    (void)iot_uc_init();

    (void)iot_ota_init();


    /* ----------------------------------------------------------------------
     * 5. Load runtime configuration
     * ---------------------------------------------------------------------- */

    iot_runtime_cfg_t cfg;

    (void)iot_config_load(&cfg);


    /* ----------------------------------------------------------------------
     * 6. Create default device ID if required
     * ---------------------------------------------------------------------- */

    if (cfg.device_id[0] == '\0' ||
        strstr(cfg.device_id, "unprovisioned") != NULL) {

        (void)iot_security_default_device_id(
            cfg.device_id,
            sizeof(cfg.device_id)
        );

        (void)iot_config_save(&cfg);
    }


    /* ----------------------------------------------------------------------
     * 7. Initialize PRODUCT PROFILE BEFORE starting tasks
     *
     * IMPORTANT:
     *
     * The sensor task can start immediately.
     * Therefore the profile hooks MUST already exist before
     * iot_tasks_start().
     * ---------------------------------------------------------------------- */

    if (iot_profile_init() != IOT_OK) {

        IOT_LOGE(TAG, "profile init failed");

        return;
    }


    /* ----------------------------------------------------------------------
     * 8. Install profile hooks BEFORE starting RTOS tasks
     * ---------------------------------------------------------------------- */

    iot_tasks_set_hooks(
        iot_profile_sensor_hook(),
        iot_profile_proc_hook(),
        iot_profile_cmd_hook()
    );


    /* ----------------------------------------------------------------------
     * 9. Print active profile
     * ---------------------------------------------------------------------- */

    IOT_LOGI(TAG,
             "profile=%s device=%s",
             iot_profile_name(),
             cfg.device_id);


    /* ----------------------------------------------------------------------
     * 10. Start RTOS task architecture
     *
     * At this point:
     *
     *     profile initialized
     *          ↓
     *     hooks installed
     *          ↓
     *     tasks started
     *          ↓
     *     sensor task can safely call profile sensor hook
     * ---------------------------------------------------------------------- */

    if (iot_tasks_start(NULL) != IOT_OK) {

        IOT_LOGE(TAG, "tasks fail");

        return;
    }


    /* ----------------------------------------------------------------------
     * 11. BLE
     *
     * BLE is always enabled for commissioning.
     * ---------------------------------------------------------------------- */

    (void)iot_ble_init(cfg.device_id);


    /* ----------------------------------------------------------------------
     * 12. Wi-Fi
     * ---------------------------------------------------------------------- */

    (void)iot_wifi_init();


    /* ----------------------------------------------------------------------
     * 13. MQTT
     * ---------------------------------------------------------------------- */

    (void)iot_mqtt_init();


    /* ----------------------------------------------------------------------
     * 14. Start Wi-Fi if device is provisioned
     * ---------------------------------------------------------------------- */

    if (cfg.provisioned &&
        cfg.wifi_ssid[0] != '\0') {

        IOT_LOGI(TAG,
                 "starting wifi ssid=%s",
                 cfg.wifi_ssid);

        (void)iot_wifi_start(
            cfg.wifi_ssid,
            cfg.wifi_pass
        );


        /* --------------------------------------------------------------
         * Wait for Wi-Fi IP
         * -------------------------------------------------------------- */

        iot_task_ctx_t *ctx = iot_tasks_ctx();

        uint32_t bits = iot_event_wait(
            ctx->system_evt,
            IOT_EVT_WIFI_IP,
            false,
            true,
            30000U
        );


        /* --------------------------------------------------------------
         * Start MQTT after Wi-Fi IP is ready
         * -------------------------------------------------------------- */

        if ((bits & IOT_EVT_WIFI_IP) != 0U) {

    IOT_LOGI(TAG,
             "wifi IP ready — starting MQTT");

    IOT_LOGI(TAG,
             "MQTT URI = [%s]",
             cfg.mqtt_uri);

    (void)iot_mqtt_start(&cfg);

        } else {

            iot_diag_event(
                DIAG_WIFI_CONNECT_FAIL
            );

            IOT_LOGW(TAG,
                     "wifi timeout — remain in BLE commissioning");
        }

    } else {

        IOT_LOGI(TAG,
                 "unprovisioned — BLE commissioning");
    }


    /* ----------------------------------------------------------------------
     * 15. Mark OTA image valid
     * ---------------------------------------------------------------------- */

    (void)iot_ota_mark_valid();


    /* ----------------------------------------------------------------------
     * 16. Boot complete
     * ---------------------------------------------------------------------- */

    IOT_LOGI(TAG,
             "boot complete");
}