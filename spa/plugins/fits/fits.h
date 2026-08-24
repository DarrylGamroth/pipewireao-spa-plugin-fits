/* SPDX-License-Identifier: MIT */
#ifndef SPA_FITS_H
#define SPA_FITS_H

#include <spa/support/log.h>
#include <spa/support/plugin.h>

#include <pipewireao-plugins/fits.h>

extern const struct spa_handle_factory spa_fits_source_factory;

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &fits_log_topic
extern struct spa_log_topic fits_log_topic;

#endif
