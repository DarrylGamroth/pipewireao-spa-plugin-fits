/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_PLUGINS_CALCULON_H
#define PIPEWIREAO_PLUGINS_CALCULON_H

#include <spa/param/props.h>

#define SPA_NAME_API_CALCULON_PIXEL_CALIBRATION \
	"api.calculon.pixel-calibration"

#define SPA_KEY_API_CALCULON_DETECTOR_SIZE "api.calculon.detector-size"
#define SPA_KEY_API_CALCULON_DETECTOR_RATE "api.calculon.detector-rate"
#define SPA_KEY_API_CALCULON_DETECTOR_PROFILE "api.calculon.detector-profile"

#define SPA_PROP_CALCULON_PIXEL_CALIBRATION_FLAT_SEQ \
	(SPA_PROP_START_CUSTOM + 0)
#define SPA_PROP_CALCULON_PIXEL_CALIBRATION_BACKGROUND_SEQ \
	(SPA_PROP_START_CUSTOM + 1)

#endif /* PIPEWIREAO_PLUGINS_CALCULON_H */
