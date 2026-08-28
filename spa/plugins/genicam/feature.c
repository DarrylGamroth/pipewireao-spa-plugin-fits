/* SPDX-License-Identifier: MIT */
#include "feature.h"

#include <stddef.h>
#include <string.h>

bool pwao_genicam_feature_changes_payload_layout(const char *name)
{
	static const char *const exact[] = {
		"PixelFormat", "Width", "Height", "PayloadSize",
		"ChunkModeActive",
	};
	static const char *const fragments[] = {
		"Binning", "Decimation", "Resolution", "Region",
		"ComponentEnable", "ChunkEnable",
	};
	size_t i;

	if (name == NULL)
		return false;
	for (i = 0; i < sizeof(exact) / sizeof(exact[0]); i++)
		if (strcmp(name, exact[i]) == 0)
			return true;
	for (i = 0; i < sizeof(fragments) / sizeof(fragments[0]); i++)
		if (strstr(name, fragments[i]) != NULL)
			return true;
	return false;
}
