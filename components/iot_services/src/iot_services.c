/**
 * @file iot_services.c
 * @copyright Copyright (c) 2026 Embedded AI Design Labs Pvt Ltd.
 * @author    Muhammad Samiullah, CTO & Founder
 * @brief Command bus: BLE/MQTT → security validate → provision/OTA/profile.
 */

#include "iot_services.h"
#include "iot_security.h"
#include "iot_provision.h"
#include "iot_ota.h"
#include "iot_log.h"
#include "iot_diag.h"
#include "iot_uc.h"
#include "iot_mqtt.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "iot_svc";

iot_err_t iot_services_init(void)
{
    return IOT_OK;
}

iot_err_t iot_services_fill_telemetry(
    iot_telemetry_msg_t *msg,
    const char *json)
{
    if ((msg == NULL) || (json == NULL)) {
        return IOT_ERR_INVALID_ARG;
    }

    size_t n = strlen(json);

    if (n >= sizeof(msg->payload)) {
        n = sizeof(msg->payload) - 1U;
    }

    (void)memcpy(msg->payload, json, n);

    msg->payload[n] = 0;
    msg->payload_len = (uint16_t)n;

    return IOT_OK;
}

iot_err_t iot_services_handle_command(
    const iot_command_msg_t *cmd)
{
    iot_err_t e = iot_security_validate_command(cmd);

    if (IOT_FAIL(e)) {
        iot_diag_event(DIAG_CMD_REJECT);
        return e;
    }

    /*
     * --------------------------------------------------------
     * Use-case command
     *
     * Example:
     * {"uc":"energy.set_limits","over_w":4000}
     *
     * or:
     * {"uc":"energy.get_state"}
     *
     * Execute the use case and publish its JSON result
     * through the existing MQTT telemetry path.
     * --------------------------------------------------------
     */

    if (iot_uc_json_has_key(
            (const char *)cmd->payload,
            "uc")) {

        iot_uc_out_t uco;

        e = iot_uc_dispatch_command(
            cmd,
            &uco);

        IOT_LOGI(
            TAG,
            "uc %s err=%d",
            iot_uc_name(uco.id),
            (int)uco.err);

        /*
         * Publish the use-case output JSON.
         *
         * This is important for commands such as
         * energy.get_state because the use-case generates
         * its result in uco.json.
         */

        if ((IOT_OK == e) &&
            (uco.json[0] != '\0')) {

            iot_telemetry_msg_t msg;

            (void)memset(
                &msg,
                0,
                sizeof(msg));

            size_t n = strlen(
                uco.json);

            if (n >= sizeof(msg.payload)) {
                n = sizeof(msg.payload) - 1U;
            }

            (void)memcpy(
                msg.payload,
                uco.json,
                n);

            msg.payload[n] = '\0';

            msg.payload_len = (uint16_t)n;

            iot_err_t pub_err =
                iot_mqtt_publish_telemetry(
                    &msg);

            IOT_LOGI(
                TAG,
                "uc response MQTT publish err=%d",
                (int)pub_err);
        }

        return e;
    }

    /*
     * --------------------------------------------------------
     * BLE provisioning / MQTT provisioning
     * --------------------------------------------------------
     */

    if ((cmd->opcode == 0xFF01) ||
        (cmd->opcode == 0x4D51 &&
         cmd->payload[2] == 's')) {

        return iot_provision_apply_json(
            (const char *)cmd->payload,
            cmd->payload_len);
    }

    /*
     * --------------------------------------------------------
     * OTA command
     * --------------------------------------------------------
     */

    if (strstr(
            (const char *)cmd->payload,
            "\"ota_url\"") != NULL) {

        char url[200];

        url[0] = '\0';

        const char *p =
            strstr(
                (const char *)cmd->payload,
                "https://");

        if (p != NULL) {

            size_t i = 0;

            while ((p[i] != '\0') &&
                   (p[i] != '"') &&
                   (i < sizeof(url) - 1U)) {

                url[i] = p[i];
                i++;
            }

            url[i] = '\0';

            return iot_ota_start(url);
        }
    }

    /*
     * --------------------------------------------------------
     * Unknown / profile command
     * --------------------------------------------------------
     */

    IOT_LOGI(
        TAG,
        "cmd opcode=0x%04x len=%u forwarded to profile",
        (unsigned)cmd->opcode,
        (unsigned)cmd->payload_len);

    return IOT_OK;
}