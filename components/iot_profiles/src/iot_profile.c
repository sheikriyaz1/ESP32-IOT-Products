/**
 * @file iot_profile.c
 * @copyright Copyright (c) 2026 Embedded AI Design Labs Pvt Ltd.
 * @author    Muhammad Samiullah, CTO & Founder
 * @brief Selects compiled-in product profile (Kconfig).
 */

#include "iot_profile.h"
#include "iot_log.h"
#include "iot_services.h"
#include "iot_provision.h"

#include <stddef.h>
#include <stdint.h>

static const char *TAG = "iot_profile";

static iot_sensor_hook_t s_sensor;
static iot_proc_hook_t s_proc;
static iot_cmd_hook_t s_cmd;

static const char *s_name = "none";

/*
 * Default processing hook.
 * If a profile does not provide a processing hook,
 * telemetry passes through unchanged.
 */
static iot_err_t passthrough_proc(
    const iot_telemetry_msg_t *in,
    iot_telemetry_msg_t *out)
{
    if ((in == NULL) || (out == NULL)) {
        return IOT_ERR_INVALID_ARG;
    }

    *out = *in;
    return IOT_OK;
}

/*
 * Common command wrapper.
 *
 * BLE FF01 = provisioning/configuration characteristic.
 *
 * FF01 payload contains JSON such as:
 * {
 *   "ssid":"MyWiFi",
 *   "pass":"MyPassword",
 *   "mqtt":"mqtts://broker.example.com:8883",
 *   "user":"user",
 *   "mpw":"password"
 * }
 *
 * Provisioning must be handled here before the
 * product-profile command handler.
 */
static iot_err_t profile_cmd_hook(const iot_command_msg_t *cmd)
{
    if (cmd == NULL) {
        return IOT_ERR_INVALID_ARG;
    }

    /*
     * BLE FF01 Config characteristic.
     */
    if (cmd->opcode == 0xFF01U) {
        if (cmd->payload_len == 0U) {
            return IOT_ERR_INVALID_ARG;
        }

        IOT_LOGI(TAG, "BLE provisioning command received");

        return iot_provision_apply_json(
            (const char *)cmd->payload,
            (size_t)cmd->payload_len);
    }

    /*
     * All other commands go to the selected product profile.
     */
    if (s_cmd != NULL) {
        return s_cmd(cmd);
    }

    return iot_services_handle_command(cmd);
}

const char *iot_profile_name(void)
{
    return s_name;
}

iot_sensor_hook_t iot_profile_sensor_hook(void)
{
    return s_sensor;
}

iot_proc_hook_t iot_profile_proc_hook(void)
{
    return s_proc != NULL ? s_proc : passthrough_proc;
}

iot_cmd_hook_t iot_profile_cmd_hook(void)
{
    /*
     * Always return our wrapper so BLE provisioning
     * can be handled independently of the selected profile.
     */
    return profile_cmd_hook;
}

iot_err_t iot_profile_dispatch_cmd(const iot_command_msg_t *cmd)
{
    return profile_cmd_hook(cmd);
}

iot_err_t iot_profile_init(void)
{
#if defined(CONFIG_IOT_PROFILE_INDUSTRIAL)

    s_name = "industrial";
    s_sensor = iot_profile_industrial_sensor;
    s_cmd = iot_profile_industrial_cmd;

    IOT_LOGI(TAG, "profile=%s", s_name);

    return iot_profile_industrial_init();

#elif defined(CONFIG_IOT_PROFILE_PREDICTIVE)

    s_name = "predictive";
    s_sensor = iot_profile_predictive_sensor;
    s_cmd = iot_profile_predictive_cmd;

    IOT_LOGI(TAG, "profile=%s", s_name);

    return iot_profile_predictive_init();

#elif defined(CONFIG_IOT_PROFILE_WATER)

    s_name = "water";
    s_sensor = iot_profile_water_sensor;
    s_cmd = iot_profile_water_cmd;

    IOT_LOGI(TAG, "profile=%s", s_name);

    return iot_profile_water_init();

#elif defined(CONFIG_IOT_PROFILE_EV_CHARGER)

    s_name = "ev_charger";
    s_sensor = iot_profile_ev_sensor;
    s_cmd = iot_profile_ev_cmd;

    IOT_LOGI(TAG, "profile=%s", s_name);

    return iot_profile_ev_init();

#elif defined(CONFIG_IOT_PROFILE_AUTOMOTIVE)

    s_name = "automotive";
    s_sensor = iot_profile_automotive_sensor;
    s_cmd = iot_profile_automotive_cmd;

    IOT_LOGI(TAG, "profile=%s", s_name);

    return iot_profile_automotive_init();

#elif defined(CONFIG_IOT_PROFILE_ASSET_TRACKER)

    s_name = "asset_tracker";
    s_sensor = iot_profile_tracker_sensor;
    s_cmd = iot_profile_tracker_cmd;

    IOT_LOGI(TAG, "profile=%s", s_name);

    return iot_profile_tracker_init();

#elif defined(CONFIG_IOT_PROFILE_AGRICULTURE)

    s_name = "agriculture";
    s_sensor = iot_profile_agri_sensor;
    s_cmd = iot_profile_agri_cmd;

    IOT_LOGI(TAG, "profile=%s", s_name);

    return iot_profile_agri_init();

#elif defined(CONFIG_IOT_PROFILE_COLDCHAIN)

    s_name = "coldchain";
    s_sensor = iot_profile_coldchain_sensor;
    s_cmd = iot_profile_coldchain_cmd;

    IOT_LOGI(TAG, "profile=%s", s_name);

    return iot_profile_coldchain_init();

#elif defined(CONFIG_IOT_PROFILE_ROBOTICS)

    s_name = "robotics";
    s_sensor = iot_profile_robotics_sensor;
    s_cmd = iot_profile_robotics_cmd;

    IOT_LOGI(TAG, "profile=%s", s_name);

    return iot_profile_robotics_init();

#else

    /*
     * Default profile.
     */
    s_name = "energy";
    s_sensor = iot_profile_energy_sensor;
    s_cmd = iot_profile_energy_cmd;

    IOT_LOGI(TAG, "profile=%s", s_name);

    return iot_profile_energy_init();

#endif
}