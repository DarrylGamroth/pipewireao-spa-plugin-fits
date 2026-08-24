/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_PLUGINS_POD_H
#define PIPEWIREAO_PLUGINS_POD_H

#include <spa/pod/builder.h>
#include <spa/pod/iter.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Copy an object POD while replacing each fixated Choice(None) property with
 * its selected value. SPA graph negotiation preserves the Choice wrapper when
 * it fixates a format; strict application parsers can use this canonical form
 * after negotiation without accepting unresolved alternatives.
 */
static inline struct spa_pod *pipewireao_pod_unwrap_fixed_choices(
		struct spa_pod_builder *builder, const struct spa_pod *source)
{
	const struct spa_pod_object *object;
	const struct spa_pod_prop *property;
	struct spa_pod_frame frame;

	if (builder == NULL || !spa_pod_is_object(source))
		return NULL;
	object = (const struct spa_pod_object *)source;
	if (spa_pod_builder_push_object(builder, &frame, object->body.type,
			object->body.id) < 0)
		return NULL;
	SPA_POD_OBJECT_FOREACH(object, property) {
		const struct spa_pod *value = &property->value;

		if (spa_pod_is_choice(value)) {
			if (SPA_POD_CHOICE_TYPE(value) != SPA_CHOICE_None ||
					SPA_POD_CHOICE_N_VALUES(value) == 0)
				return NULL;
			value = SPA_POD_CHOICE_CHILD(value);
		}
		if (spa_pod_builder_prop(builder, property->key, property->flags) < 0 ||
				spa_pod_builder_primitive(builder, value) < 0)
			return NULL;
	}
	return spa_pod_builder_pop(builder, &frame);
}

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIREAO_PLUGINS_POD_H */
