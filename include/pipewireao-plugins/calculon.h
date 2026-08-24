/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_PLUGINS_CALCULON_H
#define PIPEWIREAO_PLUGINS_CALCULON_H

#include <spa/param/props.h>

#define SPA_NAME_API_CALCULON_PIXEL_CALIBRATION \
	"api.calculon.pixel-calibration"
#define SPA_NAME_API_CALCULON_SHWFS_CONTROLLER \
	"api.calculon.shwfs-controller"
#define SPA_NAME_API_ALPAO_COMMAND_NORMALIZATION \
	"api.alpao.command-normalization"

#define SPA_KEY_API_CALCULON_DETECTOR_SIZE "api.calculon.detector-size"
#define SPA_KEY_API_CALCULON_DETECTOR_RATE "api.calculon.detector-rate"
#define SPA_KEY_API_CALCULON_DETECTOR_PROFILE "api.calculon.detector-profile"
#define SPA_KEY_API_CALCULON_REGION_SIZE "api.calculon.region-size"
#define SPA_KEY_API_CALCULON_REGION_ORIGINS "api.calculon.region-origins"
#define SPA_KEY_API_CALCULON_ACTUATOR_COUNT "api.calculon.actuator-count"
#define SPA_KEY_API_CALCULON_RECONSTRUCTION_MATRIX_PATH \
	"api.calculon.reconstruction-matrix-path"
#define SPA_KEY_API_CALCULON_COORDINATE_SCALE "api.calculon.coordinate-scale"
#define SPA_KEY_API_CALCULON_PIXEL_THRESHOLD "api.calculon.pixel-threshold"
#define SPA_KEY_API_CALCULON_FLUX_THRESHOLD "api.calculon.flux-threshold"
#define SPA_KEY_API_CALCULON_CONTROLLER_GAIN "api.calculon.controller-gain"
#define SPA_KEY_API_CALCULON_CONTROLLER_POLE "api.calculon.controller-pole"
#define SPA_KEY_API_CALCULON_COMMAND_MINIMUM "api.calculon.command-minimum"
#define SPA_KEY_API_CALCULON_COMMAND_MAXIMUM "api.calculon.command-maximum"

#define SPA_CALCULON_SCHEMA_DEMANDED_PDM_COMMAND \
	"org.calculon.ao.demanded-pdm-command/1"

#define SPA_PROP_CALCULON_PIXEL_CALIBRATION_FLAT_SEQ \
	(SPA_PROP_START_CUSTOM + 0)
#define SPA_PROP_CALCULON_PIXEL_CALIBRATION_BACKGROUND_SEQ \
	(SPA_PROP_START_CUSTOM + 1)

#endif /* PIPEWIREAO_PLUGINS_CALCULON_H */
