/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct aravis_feature {
	ArvGcFeatureNode *node;
	char *property_name;
	GPtrArray *enum_entries;
	enum aravis_feature_kind kind;
};

struct aravis_camera {
	ArvCamera *camera;
	ArvStream *stream;
	GPtrArray *features;
	struct aravis_camera_info info;
	uint32_t announced_count;
	bool started;
};

static void free_feature(gpointer data)
{
	struct aravis_feature *feature = data;

	if (feature == NULL)
		return;
	g_clear_pointer(&feature->enum_entries, g_ptr_array_unref);
	g_free(feature->property_name);
	g_free(feature);
}

static int clear_error(GError **error, int result)
{
	g_clear_error(error);
	return result;
}

static int copy_text(char *destination, size_t capacity, const char *source)
{
	int written;

	if (source == NULL)
		source = "";
	written = snprintf(destination, capacity, "%s", source);
	return written >= 0 && (size_t)written < capacity ? 0 : -ENAMETOOLONG;
}

static int query_info(struct aravis_camera *camera)
{
	GError *error = NULL;
	const char *text;
	gint x, y, width, height;
	guint payload;
	int res;

	payload = arv_camera_get_payload(camera->camera, &error);
	if (error != NULL || payload == 0)
		return clear_error(&error, -EIO);
	arv_camera_get_region(camera->camera, &x, &y, &width, &height, &error);
	if (error != NULL || x < 0 || y < 0 || width <= 0 || height <= 0)
		return clear_error(&error, -EIO);

	camera->info.payload_size = payload;
	camera->info.offset_x = (uint32_t)x;
	camera->info.offset_y = (uint32_t)y;
	camera->info.width = (uint32_t)width;
	camera->info.height = (uint32_t)height;

	text = arv_camera_get_pixel_format_as_string(camera->camera, &error);
	if (error != NULL || (res = copy_text(camera->info.pixel_format,
			sizeof(camera->info.pixel_format), text)) < 0)
		return clear_error(&error, error != NULL ? -EIO : res);
	text = arv_camera_get_model_name(camera->camera, &error);
	if (error != NULL || (res = copy_text(camera->info.model,
			sizeof(camera->info.model), text)) < 0)
		return clear_error(&error, error != NULL ? -EIO : res);
	text = arv_camera_get_device_serial_number(camera->camera, &error);
	if (error != NULL || (res = copy_text(camera->info.serial,
			sizeof(camera->info.serial), text)) < 0)
		return clear_error(&error, error != NULL ? -EIO : res);
	return 0;
}

static bool feature_kind(ArvGcFeatureNode *node,
		enum aravis_feature_kind *kind)
{
	if (ARV_IS_GC_ENUMERATION(node))
		*kind = ARAVIS_FEATURE_ENUMERATION;
	else if (ARV_IS_GC_BOOLEAN(node))
		*kind = ARAVIS_FEATURE_BOOLEAN;
	else if (ARV_IS_GC_INTEGER(node))
		*kind = ARAVIS_FEATURE_INTEGER;
	else if (ARV_IS_GC_FLOAT(node))
		*kind = ARAVIS_FEATURE_FLOATING;
	else if (ARV_IS_GC_STRING(node))
		*kind = ARAVIS_FEATURE_STRING;
	else if (ARV_IS_GC_COMMAND(node))
		*kind = ARAVIS_FEATURE_COMMAND;
	else
		return false;
	return true;
}

static void add_feature(struct aravis_camera *camera,
		ArvGcFeatureNode *node)
{
	struct aravis_feature *feature;
	const char *name;

	feature = g_new0(struct aravis_feature, 1);
	if (!feature_kind(node, &feature->kind)) {
		g_free(feature);
		return;
	}
	name = arv_gc_feature_node_get_name(node);
	if (name == NULL) {
		g_free(feature);
		return;
	}
	feature->node = node;
	feature->property_name = g_strdup_printf("genicam.%s", name);
	if (feature->kind == ARAVIS_FEATURE_ENUMERATION) {
		const GSList *entry;

		feature->enum_entries = g_ptr_array_new();
		for (entry = arv_gc_enumeration_get_entries(
				ARV_GC_ENUMERATION(node)); entry != NULL;
				entry = entry->next) {
			ArvGcFeatureNode *entry_node = entry->data;

			if (ARV_IS_GC_ENUM_ENTRY(entry_node) &&
					arv_gc_feature_node_is_implemented(entry_node, NULL))
				g_ptr_array_add(feature->enum_entries, entry_node);
		}
	}
	g_ptr_array_add(camera->features, feature);
}

static void collect_features(struct aravis_camera *camera, ArvGc *genicam,
		const char *name, GHashTable *visited)
{
	ArvGcNode *node;

	if (name == NULL || g_hash_table_contains(visited, name))
		return;
	g_hash_table_add(visited, g_strdup(name));
	node = arv_gc_get_node(genicam, name);
	if (!ARV_IS_GC_FEATURE_NODE(node) ||
			!arv_gc_feature_node_is_implemented(ARV_GC_FEATURE_NODE(node),
				NULL))
		return;
	if (ARV_IS_GC_CATEGORY(node)) {
		const GSList *feature;

		for (feature = arv_gc_category_get_features(ARV_GC_CATEGORY(node));
				feature != NULL; feature = feature->next)
			collect_features(camera, genicam, feature->data, visited);
		return;
	}
	add_feature(camera, ARV_GC_FEATURE_NODE(node));
}

static void discover_features(struct aravis_camera *camera)
{
	ArvDevice *device = arv_camera_get_device(camera->camera);
	ArvGc *genicam = arv_device_get_genicam(device);
	GHashTable *visited;

	camera->features = g_ptr_array_new_with_free_func(free_feature);
	if (genicam == NULL)
		return;
	visited = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	collect_features(camera, genicam, "Root", visited);
	g_hash_table_unref(visited);
}

int aravis_camera_open(struct aravis_camera **camera_ptr,
		const struct aravis_camera_options *options)
{
	struct aravis_camera *camera;
	GError *error = NULL;
	int res;

	if (camera_ptr == NULL || options == NULL || options->device_id == NULL)
		return -EINVAL;
	*camera_ptr = NULL;
	camera = calloc(1, sizeof(*camera));
	if (camera == NULL)
		return -errno;

	camera->camera = arv_camera_new(options->device_id, &error);
	if (camera->camera == NULL) {
		res = clear_error(&error, -ENODEV);
		goto error;
	}
	camera->stream = arv_camera_create_stream(camera->camera, NULL, NULL,
			NULL, &error);
	if (camera->stream == NULL) {
		res = clear_error(&error, -EIO);
		goto error;
	}
	if (!ARV_IS_GENTL_STREAM(camera->stream)) {
		res = -ENOTSUP;
		goto error;
	}
	if (!arv_gentl_stream_set_caller_polling(
			ARV_GENTL_STREAM(camera->stream), TRUE, &error)) {
		res = clear_error(&error, -EIO);
		goto error;
	}
	if ((res = query_info(camera)) < 0)
		goto error;
	discover_features(camera);

	*camera_ptr = camera;
	return 0;

error:
	aravis_camera_close(camera);
	return res;
}

void aravis_camera_close(struct aravis_camera *camera)
{
	if (camera == NULL)
		return;
	(void)aravis_camera_stop(camera);
	g_clear_pointer(&camera->features, g_ptr_array_unref);
	g_clear_object(&camera->stream);
	g_clear_object(&camera->camera);
	free(camera);
}

const struct aravis_camera_info *aravis_camera_get_info(
		const struct aravis_camera *camera)
{
	return camera == NULL ? NULL : &camera->info;
}

uint32_t aravis_camera_get_feature_count(const struct aravis_camera *camera)
{
	if (camera == NULL || camera->features == NULL)
		return 0;
	return camera->features->len;
}

static struct aravis_feature *get_feature(const struct aravis_camera *camera,
		uint32_t index)
{
	if (camera == NULL || camera->features == NULL ||
			index >= camera->features->len)
		return NULL;
	return g_ptr_array_index(camera->features, index);
}

static bool feature_changes_layout(const char *name)
{
	static const char *const exact[] = {
		"PixelFormat", "Width", "Height", "OffsetX", "OffsetY",
		"PayloadSize", "ChunkModeActive",
	};
	static const char *const fragments[] = {
		"Binning", "Decimation", "Resolution", "Region",
		"ComponentEnable", "ChunkEnable",
	};
	uint32_t i;

	for (i = 0; i < G_N_ELEMENTS(exact); i++)
		if (strcmp(name, exact[i]) == 0)
			return true;
	for (i = 0; i < G_N_ELEMENTS(fragments); i++)
		if (strstr(name, fragments[i]) != NULL)
			return true;
	return false;
}

int aravis_camera_get_feature_info(struct aravis_camera *camera,
		uint32_t index, struct aravis_feature_info *info)
{
	struct aravis_feature *feature = get_feature(camera, index);
	ArvGcAccessMode access;
	const char *description;
	GError *error = NULL;
	bool available, locked = false;

	if (feature == NULL || info == NULL)
		return -EINVAL;
	available = arv_gc_feature_node_is_available(feature->node, &error);
	if (error != NULL)
		return clear_error(&error, -EIO);
	if (available) {
		locked = arv_gc_feature_node_is_locked(feature->node, &error);
		if (error != NULL)
			return clear_error(&error, -EIO);
	}
	access = arv_gc_feature_node_get_actual_access_mode(feature->node);
	description = arv_gc_feature_node_get_description(feature->node);
	if (description == NULL)
		description = arv_gc_feature_node_get_tooltip(feature->node);
	if (description == NULL)
		description = arv_gc_feature_node_get_display_name(feature->node);
	if (description == NULL)
		description = arv_gc_feature_node_get_name(feature->node);
	*info = (struct aravis_feature_info) {
		.name = arv_gc_feature_node_get_name(feature->node),
		.property_name = feature->property_name,
		.description = description,
		.kind = feature->kind,
		.n_enum_entries = feature->enum_entries == NULL ? 0 :
				feature->enum_entries->len,
		.available = available,
		.readable = access == ARV_GC_ACCESS_MODE_RO ||
				access == ARV_GC_ACCESS_MODE_RW,
		.writable = !locked && (access == ARV_GC_ACCESS_MODE_WO ||
				access == ARV_GC_ACCESS_MODE_RW),
		.changes_layout = feature_changes_layout(
				arv_gc_feature_node_get_name(feature->node)),
	};
	return 0;
}

const char *aravis_camera_get_feature_enum_entry(
		const struct aravis_camera *camera, uint32_t index,
		uint32_t entry_index)
{
	struct aravis_feature *feature = get_feature(camera, index);
	ArvGcFeatureNode *entry;

	if (feature == NULL || feature->kind != ARAVIS_FEATURE_ENUMERATION ||
			feature->enum_entries == NULL ||
			entry_index >= feature->enum_entries->len)
		return NULL;
	entry = g_ptr_array_index(feature->enum_entries, entry_index);
	return arv_gc_feature_node_get_name(entry);
}

int aravis_camera_get_feature_value(struct aravis_camera *camera,
		uint32_t index, struct aravis_feature_value *value)
{
	struct aravis_feature *feature = get_feature(camera, index);
	GError *error = NULL;
	uint32_t i;

	if (feature == NULL || value == NULL ||
			feature->kind == ARAVIS_FEATURE_COMMAND)
		return -EINVAL;
	memset(value, 0, sizeof(*value));
	value->kind = feature->kind;
	switch (feature->kind) {
	case ARAVIS_FEATURE_BOOLEAN:
		value->boolean = arv_gc_boolean_get_value(
				ARV_GC_BOOLEAN(feature->node), &error);
		break;
	case ARAVIS_FEATURE_INTEGER:
		value->integer = arv_gc_integer_get_value(
				ARV_GC_INTEGER(feature->node), &error);
		break;
	case ARAVIS_FEATURE_FLOATING:
		value->floating = arv_gc_float_get_value(
				ARV_GC_FLOAT(feature->node), &error);
		break;
	case ARAVIS_FEATURE_ENUMERATION: {
		gint64 current = arv_gc_enumeration_get_int_value(
				ARV_GC_ENUMERATION(feature->node), &error);

		if (error != NULL)
			break;
		for (i = 0; i < feature->enum_entries->len; i++) {
			ArvGcEnumEntry *entry = g_ptr_array_index(
					feature->enum_entries, i);
			gint64 candidate = arv_gc_enum_entry_get_value(entry, &error);

			if (error != NULL)
				break;
			if (candidate == current) {
				value->enumeration = (int32_t)i;
				return 0;
			}
		}
		if (error == NULL)
			return -ENODATA;
		break;
	}
	case ARAVIS_FEATURE_STRING:
		value->string = arv_gc_string_get_value(
				ARV_GC_STRING(feature->node), &error);
		break;
	case ARAVIS_FEATURE_COMMAND:
		return -EINVAL;
	}
	return error == NULL ? 0 : clear_error(&error, -EIO);
}

int aravis_camera_get_feature_integer_range(struct aravis_camera *camera,
		uint32_t index, int64_t *minimum, int64_t *maximum)
{
	struct aravis_feature *feature = get_feature(camera, index);
	GError *error = NULL;

	if (feature == NULL || feature->kind != ARAVIS_FEATURE_INTEGER ||
			minimum == NULL || maximum == NULL)
		return -EINVAL;
	*minimum = arv_gc_integer_get_min(ARV_GC_INTEGER(feature->node), &error);
	if (error != NULL)
		return clear_error(&error, -EIO);
	*maximum = arv_gc_integer_get_max(ARV_GC_INTEGER(feature->node), &error);
	return error == NULL ? 0 : clear_error(&error, -EIO);
}

int aravis_camera_get_feature_float_range(struct aravis_camera *camera,
		uint32_t index, double *minimum, double *maximum)
{
	struct aravis_feature *feature = get_feature(camera, index);
	GError *error = NULL;

	if (feature == NULL || feature->kind != ARAVIS_FEATURE_FLOATING ||
			minimum == NULL || maximum == NULL)
		return -EINVAL;
	*minimum = arv_gc_float_get_min(ARV_GC_FLOAT(feature->node), &error);
	if (error != NULL)
		return clear_error(&error, -EIO);
	*maximum = arv_gc_float_get_max(ARV_GC_FLOAT(feature->node), &error);
	return error == NULL ? 0 : clear_error(&error, -EIO);
}

int aravis_camera_find_feature(const struct aravis_camera *camera,
		const char *property_name, uint32_t *index)
{
	uint32_t i;

	if (camera == NULL || property_name == NULL || index == NULL)
		return -EINVAL;
	for (i = 0; i < aravis_camera_get_feature_count(camera); i++) {
		struct aravis_feature *feature = get_feature(camera, i);

		if (strcmp(feature->property_name, property_name) == 0) {
			*index = i;
			return 0;
		}
	}
	return -ENOENT;
}

int aravis_camera_set_feature_value(struct aravis_camera *camera,
		uint32_t index, const struct aravis_feature_value *value)
{
	struct aravis_feature *feature = get_feature(camera, index);
	struct aravis_feature_info info;
	GError *error = NULL;
	int res;

	if (feature == NULL || value == NULL || value->kind != feature->kind)
		return -EINVAL;
	if (camera->started)
		return -EBUSY;
	if ((res = aravis_camera_get_feature_info(camera, index, &info)) < 0)
		return res;
	if (!info.available)
		return -ENODATA;
	if (!info.writable)
		return -EACCES;
	switch (feature->kind) {
	case ARAVIS_FEATURE_BOOLEAN:
		arv_gc_boolean_set_value(ARV_GC_BOOLEAN(feature->node),
				value->boolean, &error);
		break;
	case ARAVIS_FEATURE_INTEGER:
		arv_gc_integer_set_value(ARV_GC_INTEGER(feature->node),
				value->integer, &error);
		break;
	case ARAVIS_FEATURE_FLOATING:
		arv_gc_float_set_value(ARV_GC_FLOAT(feature->node),
				value->floating, &error);
		break;
	case ARAVIS_FEATURE_ENUMERATION: {
		ArvGcEnumEntry *entry;
		gint64 enum_value;

		if (value->enumeration < 0 || feature->enum_entries == NULL ||
				(uint32_t)value->enumeration >= feature->enum_entries->len)
			return -EINVAL;
		entry = g_ptr_array_index(feature->enum_entries,
				(uint32_t)value->enumeration);
		if (!arv_gc_feature_node_is_available(ARV_GC_FEATURE_NODE(entry),
				&error))
			return error == NULL ? -ENODATA : clear_error(&error, -EIO);
		enum_value = arv_gc_enum_entry_get_value(entry, &error);
		if (error == NULL)
			arv_gc_enumeration_set_int_value(
					ARV_GC_ENUMERATION(feature->node), enum_value, &error);
		break;
	}
	case ARAVIS_FEATURE_STRING:
		if (value->string == NULL)
			return -EINVAL;
		arv_gc_string_set_value(ARV_GC_STRING(feature->node),
				value->string, &error);
		break;
	case ARAVIS_FEATURE_COMMAND:
		return -EINVAL;
	}
	return error == NULL ? 0 : clear_error(&error, -EIO);
}

int aravis_camera_refresh_info(struct aravis_camera *camera)
{
	if (camera == NULL)
		return -EINVAL;
	if (camera->started || camera->announced_count != 0)
		return -EBUSY;
	return query_info(camera);
}

int aravis_camera_announce(struct aravis_camera *camera, void *memory,
		uint64_t size, void *user_data, ArvBuffer **buffer_ptr)
{
	ArvBuffer *buffer;
	GError *error = NULL;

	if (camera == NULL || memory == NULL || size == 0 ||
			buffer_ptr == NULL || *buffer_ptr != NULL || camera->started)
		return -EINVAL;
	buffer = arv_buffer_new_full((size_t)size, memory, user_data, NULL);
	if (buffer == NULL)
		return -ENOMEM;
	if (!arv_gentl_stream_prepare_buffer(ARV_GENTL_STREAM(camera->stream),
			buffer, &error)) {
		g_object_unref(buffer);
		return clear_error(&error, -EIO);
	}
	*buffer_ptr = buffer;
	camera->announced_count++;
	return 0;
}

int aravis_camera_revoke(struct aravis_camera *camera, ArvBuffer **buffer)
{
	if (camera == NULL || buffer == NULL || *buffer == NULL || camera->started)
		return -EINVAL;
	g_clear_object(buffer);
	camera->announced_count--;
	return 0;
}

int aravis_camera_start(struct aravis_camera *camera)
{
	GError *error = NULL;

	if (camera == NULL)
		return -EINVAL;
	if (camera->started)
		return 0;
	if (!arv_camera_start_acquisition(camera->camera, &error))
		return clear_error(&error, -EIO);
	camera->started = true;
	return 0;
}

int aravis_camera_stop(struct aravis_camera *camera)
{
	GError *error = NULL;

	if (camera == NULL)
		return -EINVAL;
	if (!camera->started)
		return 0;
	if (!arv_camera_stop_acquisition(camera->camera, &error))
		return clear_error(&error, -EIO);
	camera->started = false;
	return 0;
}

int aravis_camera_queue(struct aravis_camera *camera, ArvBuffer *buffer)
{
	if (camera == NULL || buffer == NULL)
		return -EINVAL;
	return arv_gentl_stream_queue_buffer(ARV_GENTL_STREAM(camera->stream),
			buffer, NULL) ? 0 : -EIO;
}

int aravis_camera_try_get_completion(struct aravis_camera *camera,
		struct aravis_camera_completion *completion)
{
	ArvGenTLStreamPollResult poll;
	ArvBuffer *buffer;
	const void *base, *image;
	size_t size_filled, image_size;
	gint x, y, width, height, x_padding, y_padding;

	if (camera == NULL || completion == NULL || !camera->started)
		return -EINVAL;
	memset(completion, 0, sizeof(*completion));
	poll = arv_gentl_stream_poll_buffer(ARV_GENTL_STREAM(camera->stream),
			&buffer, NULL);
	if (poll == ARV_GENTL_STREAM_POLL_EMPTY)
		return 0;
	if (poll != ARV_GENTL_STREAM_POLL_BUFFER)
		return -EIO;

	base = arv_buffer_get_data(buffer, &size_filled);
	image = arv_buffer_get_image_data(buffer, &image_size);
	arv_buffer_get_image_region(buffer, &x, &y, &width, &height);
	arv_buffer_get_image_padding(buffer, &x_padding, &y_padding);
	if (base == NULL || image == NULL || (uintptr_t)image < (uintptr_t)base ||
			x < 0 || y < 0 ||
			width < 0 || height < 0 || x_padding < 0 || y_padding < 0 ||
			(uintptr_t)image - (uintptr_t)base > UINT32_MAX ||
			image_size > UINT32_MAX)
		return -EPROTO;

	completion->buffer = buffer;
	completion->user_data = (void *)arv_buffer_get_user_data(buffer);
	completion->frame = (struct aravis_frame_info) {
		.frame_id = arv_buffer_get_frame_id(buffer),
		.camera_timestamp_ns = arv_buffer_get_timestamp(buffer),
		.size_filled = size_filled,
		.width = (uint32_t)width,
		.height = (uint32_t)height,
		.offset_x = (uint32_t)x,
		.offset_y = (uint32_t)y,
		.x_padding = (uint32_t)x_padding,
		.y_padding = (uint32_t)y_padding,
		.image_offset = (uint32_t)((uintptr_t)image - (uintptr_t)base),
		.image_size = (uint32_t)image_size,
		.incomplete = arv_buffer_get_status(buffer) != ARV_BUFFER_STATUS_SUCCESS,
	};
	completion->result = 0;
	return 1;
}
