/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "feature.hpp"

#include <spa/utils/defs.h>

using namespace egrabber_pipewire;

int main()
{
	Feature feature;
	for (const char *name : { "Width", "Height", "PixelFormat" }) {
		feature.name = name;
		spa_assert_se(changes_payload_layout(feature));
	}
	for (const char *name : { "OffsetX", "OffsetY", "ExposureTime" }) {
		feature.name = name;
		spa_assert_se(!changes_payload_layout(feature));
	}
	return 0;
}
