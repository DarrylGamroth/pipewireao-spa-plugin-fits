/* SPDX-License-Identifier: MIT */
#ifndef SPA_HERMES_H
#define SPA_HERMES_H

#include <spa/support/log.h>
#include <spa/support/plugin.h>

#define SPA_NAME_API_HERMES_SOURCE "api.hermes.source"
#define SPA_NAME_API_HERMES_DECODER "api.hermes.decoder"

#define SPA_KEY_API_HERMES_DEVICE_ID "api.hermes.device-id"
#define SPA_KEY_API_HERMES_CAMERA_MODE "api.hermes.camera-mode"
#define SPA_KEY_API_HERMES_EXPOSURE_CLOCKS "api.hermes.exposure-clocks"
#define SPA_KEY_API_HERMES_INTEGRATED_FRAMES "api.hermes.integrated-frames"
#define SPA_KEY_API_HERMES_COUNTERS "api.hermes.counters"
#define SPA_KEY_API_HERMES_FORCE_8BIT "api.hermes.force-8bit"
#define SPA_KEY_API_HERMES_HALF_ARRAY "api.hermes.half-array"
#define SPA_KEY_API_HERMES_SIGNED_DATA "api.hermes.signed-data"
#define SPA_KEY_API_HERMES_BITS_PER_PIXEL "api.hermes.bits-per-pixel"
#define SPA_KEY_API_HERMES_FRAMES_PER_BUFFER "api.hermes.frames-per-buffer"
#define SPA_KEY_API_HERMES_BATCH_RATE "api.hermes.batch-rate"
#define SPA_KEY_API_HERMES_READINESS "api.hermes.readiness"

#define SPA_HERMES_RAW_BATCH_SCHEMA \
	"org.pipewireao.hermes.frontpanel-raw-batch/1"
#define SPA_HERMES_DECODED_BATCH_SCHEMA \
	"org.pipewireao.hermes.counter-frame-batch/1"

extern const struct spa_handle_factory spa_hermes_source_factory;

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &hermes_log_topic
extern struct spa_log_topic hermes_log_topic;

#endif
