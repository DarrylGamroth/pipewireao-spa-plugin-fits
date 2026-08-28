/* SPDX-License-Identifier: MIT */
#include "feature.h"

#include <spa/utils/defs.h>

int main(void)
{
	static const char *const layout[] = {
		"PixelFormat", "Width", "Height", "PayloadSize",
		"ChunkModeActive", "BinningHorizontal", "DecimationVertical",
		"RegionSelector", "ComponentEnable", "ChunkEnable",
	};
	static const char *const live[] = {
		"OffsetX", "OffsetY", "ExposureTime", "Gain",
		"AcquisitionFrameRate", "TriggerMode",
	};

	for (size_t i = 0; i < SPA_N_ELEMENTS(layout); i++)
		spa_assert_se(pwao_genicam_feature_changes_payload_layout(layout[i]));
	for (size_t i = 0; i < SPA_N_ELEMENTS(live); i++)
		spa_assert_se(!pwao_genicam_feature_changes_payload_layout(live[i]));
	return 0;
}
