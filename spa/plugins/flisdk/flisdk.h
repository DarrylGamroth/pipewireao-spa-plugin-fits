/* SPDX-License-Identifier: MIT */
#ifndef SPA_FLISDK_H
#define SPA_FLISDK_H

#include <spa/support/log.h>
#include <spa/support/plugin.h>

#define SPA_NAME_API_FLISDK_SOURCE "api.flisdk.source"

#define SPA_KEY_API_FLISDK_CAMERA "api.flisdk.camera"
#define SPA_KEY_API_FLISDK_GRABBER "api.flisdk.grabber"
#define SPA_KEY_API_FLISDK_READINESS "api.flisdk.readiness"
#define SPA_KEY_API_FLISDK_PIXEL_SIGN "api.flisdk.pixel-sign"
#define SPA_KEY_API_FLISDK_CONTROL "api.flisdk.control"
#define SPA_KEY_API_FLISDK_CLPROTOCOL_LIBRARIES "api.flisdk.clprotocol-libraries"
#define SPA_KEY_API_FLISDK_CLPROTOCOL_DEVICE "api.flisdk.clprotocol-device"
#define SPA_KEY_API_FLISDK_CAMERA_SERIAL "api.flisdk.camera-serial"
#define SPA_KEY_API_FLISDK_GENAPI_RUNTIME "api.flisdk.genapi-runtime"
#define SPA_KEY_API_FLISDK_CONTROL_TIMEOUT_MS "api.flisdk.control-timeout-ms"

extern const struct spa_handle_factory spa_flisdk_source_factory;

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &flisdk_log_topic
extern struct spa_log_topic flisdk_log_topic;

#endif
