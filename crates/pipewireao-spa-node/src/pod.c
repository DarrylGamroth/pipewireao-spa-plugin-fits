/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <pipewireao-plugins/pod.h>

size_t pipewireao_spa_unwrap_fixed_pod(const struct spa_pod *source,
		void *storage, size_t size)
{
	struct spa_pod_builder builder;
	struct spa_pod *result;

	if (source == NULL || storage == NULL || size > UINT32_MAX)
		return 0;
	spa_pod_builder_init(&builder, storage, (uint32_t)size);
	result = pipewireao_pod_unwrap_fixed_choices(&builder, source);
	return result == NULL ? 0 : SPA_POD_SIZE(result);
}
