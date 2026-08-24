/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <errno.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <spa/buffer/image-source.h>
#include <spa/utils/ringbuffer.h>

template <typename Operation>
static BGAPI2_RESULT guarded_call(Operation operation) noexcept
{
	try {
		return operation();
	} catch (...) {
		return BGAPI2_RESULT_LOWLEVEL_ERROR;
	}
}

#define BGAPI2_CALL(expression) guarded_call([&]() { return (expression); })

static int fail(BGAPI2_RESULT result);

struct feature_record {
	BGAPI2_Node *node = nullptr;
	std::string name;
	std::string property_name;
	std::string description;
	enum bgapi2_feature_kind kind = BGAPI2_FEATURE_STRING;
	std::vector<std::string> enum_entries;
	std::string value_storage;
};

struct feature_store {
	std::vector<feature_record> features;
};

struct bgapi2_camera {
	BGAPI2_System *system;
	BGAPI2_Interface *interface;
	BGAPI2_Device *device;
	BGAPI2_DataStream *stream;
	BGAPI2_Node *acquisition_start;
	BGAPI2_Node *acquisition_stop;
	BGAPI2_Node *acquisition_abort;
	struct feature_store *feature_store;
	struct bgapi2_camera_info info;
	struct spa_ringbuffer_shared completion_ring;
	struct bgapi2_camera_completion completions[SPA_IMAGE_SOURCE_MAX_BUFFERS];
	uint32_t announced_count;
	uint32_t completion_error;
	enum bgapi2_camera_completion_mode completion_mode;
	bool system_open;
	bool interface_open;
	bool device_open;
	bool stream_open;
	bool event_handler;
	bool acquiring;
};

typedef BGAPI2_RESULT (BGAPI2CALL *node_string_getter)(BGAPI2_Node *, char *,
		bo_uint64 *);

static int get_node_text(BGAPI2_Node *node, node_string_getter getter,
		std::string &value)
{
	bo_uint64 length = 0;
	BGAPI2_RESULT result;

	result = guarded_call([&]() { return getter(node, nullptr, &length); });
	if (result != BGAPI2_RESULT_SUCCESS)
		return fail(result);
	if (length == 0) {
		value.clear();
		return 0;
	}
	value.resize(static_cast<size_t>(length));
	result = guarded_call([&]() { return getter(node, value.data(), &length); });
	if (result != BGAPI2_RESULT_SUCCESS)
		return fail(result);
	if (!value.empty() && value.back() == '\0')
		value.pop_back();
	return 0;
}

static int read_completion(struct bgapi2_camera *camera, BGAPI2_Buffer *buffer,
		struct bgapi2_camera_completion *completion);

static void BGAPI2CALL buffer_complete(void *owner, BGAPI2_Buffer *buffer)
{
	struct bgapi2_camera *camera = static_cast<struct bgapi2_camera *>(owner);
	struct bgapi2_camera_completion completion = {};
	uint32_t index;
	int32_t filled;

	filled = spa_ringbuffer_shared_get_write_index(
			&camera->completion_ring, &index);
	if (buffer == NULL || filled < 0 ||
			filled >= (int32_t)SPA_IMAGE_SOURCE_MAX_BUFFERS) {
		__atomic_store_n(&camera->completion_error, 1u, __ATOMIC_RELEASE);
		return;
	}
	completion.buffer = buffer;
	completion.result = read_completion(camera, buffer, &completion);
	camera->completions[index % SPA_IMAGE_SOURCE_MAX_BUFFERS] = completion;
	spa_ringbuffer_shared_write_update(&camera->completion_ring, index + 1u);
}

static int fail(BGAPI2_RESULT result)
{
	switch (result) {
	case BGAPI2_RESULT_ACCESS_DENIED:
		return -EACCES;
	case BGAPI2_RESULT_RESOURCE_IN_USE:
		return -EBUSY;
	case BGAPI2_RESULT_INVALID_PARAMETER:
	case BGAPI2_RESULT_INVALID_HANDLE:
	case BGAPI2_RESULT_INVALID_BUFFER:
		return -EINVAL;
	case BGAPI2_RESULT_NOT_IMPLEMENTED:
		return -ENOTSUP;
	case BGAPI2_RESULT_NOT_AVAILABLE:
	case BGAPI2_RESULT_NO_DATA:
		return -ENODATA;
	case BGAPI2_RESULT_TIMEOUT:
		return -ETIMEDOUT;
	default:
		return -EIO;
	}
}

static int checked_result(struct bgapi2_camera *, BGAPI2_RESULT result)
{
	if (result != BGAPI2_RESULT_SUCCESS)
		return fail(result);
	return 0;
}

#define checked(camera, expression) \
	checked_result((camera), BGAPI2_CALL(expression))

static bool feature_changes_layout(std::string_view name)
{
	static constexpr std::string_view exact[] = {
		"PixelFormat", "Width", "Height", "OffsetX", "OffsetY",
		"PayloadSize", "ChunkModeActive",
	};
	static constexpr std::string_view fragments[] = {
		"Binning", "Decimation", "Resolution", "Region",
		"ComponentEnable", "ChunkEnable",
	};

	if (std::find(std::begin(exact), std::end(exact), name) != std::end(exact))
		return true;
	return std::any_of(std::begin(fragments), std::end(fragments),
			[name](std::string_view fragment) {
				return name.find(fragment) != std::string_view::npos;
			});
}

static bool feature_kind(const std::string &interface,
		enum bgapi2_feature_kind &kind)
{
	if (interface == BGAPI2_NODEINTERFACE_BOOLEAN)
		kind = BGAPI2_FEATURE_BOOLEAN;
	else if (interface == BGAPI2_NODEINTERFACE_INTEGER)
		kind = BGAPI2_FEATURE_INTEGER;
	else if (interface == BGAPI2_NODEINTERFACE_FLOAT)
		kind = BGAPI2_FEATURE_FLOATING;
	else if (interface == BGAPI2_NODEINTERFACE_ENUMERATION)
		kind = BGAPI2_FEATURE_ENUMERATION;
	else if (interface == BGAPI2_NODEINTERFACE_STRING)
		kind = BGAPI2_FEATURE_STRING;
	else if (interface == BGAPI2_NODEINTERFACE_COMMAND)
		kind = BGAPI2_FEATURE_COMMAND;
	else
		return false;
	return true;
}

static int discover_enum_entries(struct bgapi2_camera *camera,
		feature_record &feature)
{
	BGAPI2_NodeMap *node_map = nullptr;
	bo_uint64 count = 0;
	int res;

	if ((res = checked(camera, BGAPI2_Node_GetEnumNodeList(feature.node,
			&node_map))) < 0 ||
			(res = checked(camera, BGAPI2_NodeMap_GetNodeCount(node_map,
			&count))) < 0)
		return res;
	for (bo_uint64 i = 0; i < count; i++) {
		BGAPI2_Node *entry = nullptr;
		bo_bool implemented = 0, available = 0;
		std::string name;

		if (checked(camera, BGAPI2_NodeMap_GetNodeByIndex(node_map, i,
				&entry)) < 0 ||
				checked(camera, BGAPI2_Node_GetImplemented(entry,
				&implemented)) < 0 || !implemented ||
				checked(camera, BGAPI2_Node_GetAvailable(entry,
				&available)) < 0 || !available ||
				get_node_text(entry, BGAPI2_Node_GetValue, name) < 0)
			continue;
		feature.enum_entries.push_back(std::move(name));
	}
	return 0;
}

static int discover_features(struct bgapi2_camera *camera)
{
	BGAPI2_NodeMap *node_map = nullptr;
	bo_uint64 count = 0;
	auto *store = new (std::nothrow) feature_store;
	int res;

	if (store == nullptr)
		return -ENOMEM;
	try {
		if ((res = checked(camera, BGAPI2_Device_GetRemoteNodeList(
				camera->device, &node_map))) < 0 ||
				(res = checked(camera, BGAPI2_NodeMap_GetNodeCount(node_map,
				&count))) < 0)
			goto error;
		store->features.reserve(static_cast<size_t>(count));
		for (bo_uint64 i = 0; i < count; i++) {
			BGAPI2_Node *node = nullptr;
			bo_bool implemented = 0;
			feature_record feature;
			std::string interface;

			if (checked(camera, BGAPI2_NodeMap_GetNodeByIndex(node_map, i,
					&node)) < 0 ||
					checked(camera, BGAPI2_Node_GetImplemented(node,
					&implemented)) < 0 || !implemented ||
					get_node_text(node, BGAPI2_Node_GetInterface,
					interface) < 0 ||
					!feature_kind(interface, feature.kind) ||
					get_node_text(node, BGAPI2_Node_GetName,
					feature.name) < 0 || feature.name.empty())
				continue;
			feature.node = node;
			feature.property_name = "genicam." + feature.name;
			if (get_node_text(node, BGAPI2_Node_GetDescription,
					feature.description) < 0 || feature.description.empty())
				feature.description = feature.name;
			if (feature.kind == BGAPI2_FEATURE_ENUMERATION &&
					discover_enum_entries(camera, feature) < 0)
				continue;
			store->features.push_back(std::move(feature));
		}
	} catch (...) {
		res = -ENOMEM;
		goto error;
	}
	camera->feature_store = store;
	return 0;

error:
	delete store;
	return res;
}

static int enable_completion_handler(struct bgapi2_camera *camera)
{
	int res;

	if (camera->event_handler)
		return 0;
	if ((res = checked(camera, BGAPI2_DataStream_SetNewBufferEventMode(
			camera->stream, EVENTMODE_EVENT_HANDLER))) < 0)
		return res;
	camera->event_handler = true;
	if ((res = checked(camera, BGAPI2_DataStream_RegisterNewBufferEventHandler(
			camera->stream, camera, buffer_complete))) < 0) {
		(void) checked(camera, BGAPI2_DataStream_SetNewBufferEventMode(
				camera->stream, EVENTMODE_POLLING));
		camera->event_handler = false;
		return res;
	}
	return 0;
}

static int disable_completion_handler(struct bgapi2_camera *camera)
{
	int res;

	if (!camera->event_handler)
		return 0;
	res = checked(camera, BGAPI2_DataStream_SetNewBufferEventMode(
			camera->stream, EVENTMODE_POLLING));
	if (res == 0)
		camera->event_handler = false;
	return res;
}

static int get_node_int(struct bgapi2_camera *camera, const char *name,
		uint64_t *value)
{
	BGAPI2_Node *node = NULL;
	bo_int64 result = 0;
	int res;

	if ((res = checked(camera, BGAPI2_Device_GetRemoteNode(
			camera->device, name, &node))) < 0 ||
			(res = checked(camera, BGAPI2_Node_GetInt(node, &result))) < 0)
		return res;
	if (result < 0)
		return -ERANGE;
	*value = (uint64_t)result;
	return 0;
}

static int get_node_string(struct bgapi2_camera *camera, const char *name,
		char *value, size_t capacity)
{
	BGAPI2_Node *node = NULL;
	bo_uint64 length = capacity;
	int res;

	if ((res = checked(camera, BGAPI2_Device_GetRemoteNode(
			camera->device, name, &node))) < 0)
		return res;
	return checked(camera, BGAPI2_Node_GetString(node, value, &length));
}

static void close_interface(struct bgapi2_camera *camera)
{
	if (camera->interface_open)
		BGAPI2_CALL(BGAPI2_Interface_Close(camera->interface));
	camera->interface = NULL;
	camera->interface_open = false;
}

static int find_device(struct bgapi2_camera *camera,
		const struct bgapi2_camera_options *options)
{
	bo_uint n_interfaces = 0;
	bo_bool changed = 0;
	uint32_t first, end, index;
	int res;

	if ((res = checked(camera, BGAPI2_System_UpdateInterfaceList(
			camera->system, &changed, options->interface_timeout_ms))) < 0 ||
			(res = checked(camera, BGAPI2_System_GetNumInterfaces(
			camera->system, &n_interfaces))) < 0)
		return res;
	if (options->interface_index == BGAPI2_CAMERA_ANY_INTERFACE) {
		first = 0;
		end = n_interfaces;
	} else {
		if (options->interface_index >= n_interfaces)
			return -ENODEV;
		first = options->interface_index;
		end = first + 1;
	}
	for (index = first; index < end; index++) {
		bo_uint n_devices = 0;

		if (checked(camera, BGAPI2_System_GetInterface(camera->system,
				index, &camera->interface)) < 0 ||
				checked(camera, BGAPI2_Interface_Open(camera->interface)) < 0) {
			camera->interface = NULL;
			continue;
		}
		camera->interface_open = true;
		if (checked(camera, BGAPI2_Interface_UpdateDeviceList(
				camera->interface, &changed, options->device_timeout_ms)) == 0 &&
				checked(camera, BGAPI2_Interface_GetNumDevices(
				camera->interface, &n_devices)) == 0 &&
				options->device_index < n_devices &&
				checked(camera, BGAPI2_Interface_GetDevice(camera->interface,
				options->device_index, &camera->device)) == 0) {
			camera->info.interface_index = index;
			camera->info.device_index = options->device_index;
			return 0;
		}
		close_interface(camera);
	}
	return -ENODEV;
}

static int query_info(struct bgapi2_camera *camera)
{
	bo_uint64 length;
	int res;

	if ((res = checked(camera, BGAPI2_Device_GetPayloadSize(
			camera->device, &camera->info.payload_size))) < 0 ||
			(res = get_node_int(camera, "Width", &camera->info.width)) < 0 ||
			(res = get_node_int(camera, "Height", &camera->info.height)) < 0 ||
			(res = get_node_int(camera, "OffsetX", &camera->info.offset_x)) < 0 ||
			(res = get_node_int(camera, "OffsetY", &camera->info.offset_y)) < 0 ||
			(res = get_node_string(camera, "PixelFormat", camera->info.pixel_format,
				sizeof(camera->info.pixel_format))) < 0)
		return res;
	length = sizeof(camera->info.model);
	if ((res = checked(camera, BGAPI2_Device_GetModel(camera->device,
			camera->info.model, &length))) < 0)
		return res;
	length = sizeof(camera->info.serial);
	return checked(camera, BGAPI2_Device_GetSerialNumber(camera->device,
			camera->info.serial, &length));
}

int bgapi2_camera_open(struct bgapi2_camera **camera_ptr,
		const struct bgapi2_camera_options *options)
{
	struct bgapi2_camera *camera;
	void *storage;
	int res;

	if (camera_ptr == NULL || options == NULL || options->producer_path == NULL)
		return -EINVAL;
	if (options->completion_mode != BGAPI2_CAMERA_COMPLETION_CALLBACK &&
			options->completion_mode != BGAPI2_CAMERA_COMPLETION_POLLING)
		return -EINVAL;
	*camera_ptr = NULL;
	if (posix_memalign(&storage, SPA_CACHE_LINE_SIZE, sizeof(*camera)) != 0)
		return -ENOMEM;
	camera = static_cast<struct bgapi2_camera *>(storage);
	memset(camera, 0, sizeof(*camera));
	spa_ringbuffer_shared_init(&camera->completion_ring);
	camera->completion_mode = options->completion_mode;
	if ((res = checked(camera, BGAPI2_LoadSystemFromPath(
			options->producer_path, &camera->system))) < 0 ||
			(res = checked(camera, BGAPI2_System_Open(camera->system))) < 0)
		goto error;
	camera->system_open = true;
	if ((res = find_device(camera, options)) < 0 ||
			(res = checked(camera, BGAPI2_Device_Open(camera->device))) < 0)
		goto error;
	camera->device_open = true;
	if ((res = checked(camera, BGAPI2_Device_GetDataStream(camera->device,
			options->stream_index, &camera->stream))) < 0 ||
			(res = checked(camera, BGAPI2_DataStream_Open(camera->stream))) < 0)
		goto error;
	camera->stream_open = true;
	camera->info.stream_index = options->stream_index;
	if ((res = query_info(camera)) < 0 ||
			(res = checked(camera, BGAPI2_Device_GetRemoteNode(camera->device,
			"AcquisitionStart", &camera->acquisition_start))) < 0 ||
			(res = checked(camera, BGAPI2_Device_GetRemoteNode(camera->device,
			"AcquisitionStop", &camera->acquisition_stop))) < 0 ||
			(res = discover_features(camera)) < 0)
		goto error;
	if (BGAPI2_CALL(BGAPI2_Device_GetRemoteNode(camera->device, "AcquisitionAbort",
			&camera->acquisition_abort)) != BGAPI2_RESULT_SUCCESS)
		camera->acquisition_abort = NULL;
	*camera_ptr = camera;
	return 0;

error:
	bgapi2_camera_close(camera);
	return res;
}

void bgapi2_camera_close(struct bgapi2_camera *camera)
{
	if (camera == NULL)
		return;
	if (camera->acquiring)
		bgapi2_camera_stop(camera);
	(void) disable_completion_handler(camera);
	if (camera->stream_open)
		BGAPI2_CALL(BGAPI2_DataStream_Close(camera->stream));
	delete camera->feature_store;
	camera->feature_store = nullptr;
	if (camera->device_open)
		BGAPI2_CALL(BGAPI2_Device_Close(camera->device));
	close_interface(camera);
	if (camera->system_open)
		BGAPI2_CALL(BGAPI2_System_Close(camera->system));
	if (camera->system != NULL)
		BGAPI2_CALL(BGAPI2_ReleaseSystem(camera->system));
	free(camera);
}

const struct bgapi2_camera_info *bgapi2_camera_get_info(
		const struct bgapi2_camera *camera)
{
	return camera == NULL ? NULL : &camera->info;
}

uint32_t bgapi2_camera_get_feature_count(const struct bgapi2_camera *camera)
{
	if (camera == nullptr || camera->feature_store == nullptr)
		return 0;
	const size_t count = camera->feature_store->features.size();
	return count > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(count);
}

static feature_record *get_feature(struct bgapi2_camera *camera,
		uint32_t index)
{
	if (camera == nullptr || camera->feature_store == nullptr ||
			index >= camera->feature_store->features.size())
		return nullptr;
	return &camera->feature_store->features[index];
}

static const feature_record *get_feature(const struct bgapi2_camera *camera,
		uint32_t index)
{
	return get_feature(const_cast<struct bgapi2_camera *>(camera), index);
}

int bgapi2_camera_get_feature_info(struct bgapi2_camera *camera,
		uint32_t index, struct bgapi2_feature_info *info)
{
	feature_record *feature = get_feature(camera, index);
	bo_bool available = 0, readable = 0, writable = 0;
	int res;

	if (feature == nullptr || info == nullptr)
		return -EINVAL;
	if ((res = checked(camera, BGAPI2_Node_GetAvailable(feature->node,
			&available))) < 0 ||
			(res = checked(camera, BGAPI2_Node_IsReadable(feature->node,
			&readable))) < 0 ||
			(res = checked(camera, BGAPI2_Node_IsWriteable(feature->node,
			&writable))) < 0)
		return res;
	*info = (struct bgapi2_feature_info) {
		.name = feature->name.c_str(),
		.property_name = feature->property_name.c_str(),
		.description = feature->description.c_str(),
		.kind = feature->kind,
		.n_enum_entries = static_cast<uint32_t>(feature->enum_entries.size()),
		.available = available != 0,
		.readable = readable != 0,
		.writable = writable != 0,
		.changes_layout = feature_changes_layout(feature->name),
	};
	return 0;
}

const char *bgapi2_camera_get_feature_enum_entry(
		const struct bgapi2_camera *camera, uint32_t index,
		uint32_t entry_index)
{
	const feature_record *feature = get_feature(camera, index);

	if (feature == nullptr || feature->kind != BGAPI2_FEATURE_ENUMERATION ||
			entry_index >= feature->enum_entries.size())
		return nullptr;
	return feature->enum_entries[entry_index].c_str();
}

int bgapi2_camera_get_feature_value(struct bgapi2_camera *camera,
		uint32_t index, struct bgapi2_feature_value *value)
{
	feature_record *feature = get_feature(camera, index);
	int res;

	if (feature == nullptr || value == nullptr ||
			feature->kind == BGAPI2_FEATURE_COMMAND)
		return -EINVAL;
	memset(value, 0, sizeof(*value));
	value->kind = feature->kind;
	switch (feature->kind) {
	case BGAPI2_FEATURE_BOOLEAN: {
		bo_bool result = 0;
		if ((res = checked(camera, BGAPI2_Node_GetBool(feature->node,
				&result))) < 0)
			return res;
		value->boolean = result != 0;
		return 0;
	}
	case BGAPI2_FEATURE_INTEGER:
		return checked(camera, BGAPI2_Node_GetInt(feature->node,
				&value->integer));
	case BGAPI2_FEATURE_FLOATING:
		return checked(camera, BGAPI2_Node_GetDouble(feature->node,
				&value->floating));
	case BGAPI2_FEATURE_ENUMERATION:
	case BGAPI2_FEATURE_STRING:
		try {
			if ((res = get_node_text(feature->node, BGAPI2_Node_GetString,
					feature->value_storage)) < 0)
				return res;
		} catch (...) {
			return -ENOMEM;
		}
		if (feature->kind == BGAPI2_FEATURE_STRING) {
			value->string = feature->value_storage.c_str();
			return 0;
		}
		for (uint32_t i = 0; i < feature->enum_entries.size(); i++)
			if (feature->enum_entries[i] == feature->value_storage) {
				value->enumeration = static_cast<int32_t>(i);
				return 0;
			}
		return -ENODATA;
	case BGAPI2_FEATURE_COMMAND:
		return -EINVAL;
	}
	return -EINVAL;
}

int bgapi2_camera_get_feature_integer_range(struct bgapi2_camera *camera,
		uint32_t index, int64_t *minimum, int64_t *maximum)
{
	feature_record *feature = get_feature(camera, index);
	int res;

	if (feature == nullptr || minimum == nullptr || maximum == nullptr ||
			feature->kind != BGAPI2_FEATURE_INTEGER)
		return -EINVAL;
	if ((res = checked(camera, BGAPI2_Node_GetIntMin(feature->node,
			minimum))) < 0)
		return res;
	return checked(camera, BGAPI2_Node_GetIntMax(feature->node, maximum));
}

int bgapi2_camera_get_feature_float_range(struct bgapi2_camera *camera,
		uint32_t index, double *minimum, double *maximum)
{
	feature_record *feature = get_feature(camera, index);
	int res;

	if (feature == nullptr || minimum == nullptr || maximum == nullptr ||
			feature->kind != BGAPI2_FEATURE_FLOATING)
		return -EINVAL;
	if ((res = checked(camera, BGAPI2_Node_GetDoubleMin(feature->node,
			minimum))) < 0)
		return res;
	return checked(camera, BGAPI2_Node_GetDoubleMax(feature->node, maximum));
}

int bgapi2_camera_find_feature(const struct bgapi2_camera *camera,
		const char *property_name, uint32_t *index)
{
	if (camera == nullptr || camera->feature_store == nullptr ||
			property_name == nullptr || index == nullptr)
		return -EINVAL;
	for (size_t i = 0; i < camera->feature_store->features.size(); i++)
		if (camera->feature_store->features[i].property_name == property_name) {
			*index = static_cast<uint32_t>(i);
			return 0;
		}
	return -ENOENT;
}

int bgapi2_camera_set_feature_value(struct bgapi2_camera *camera,
		uint32_t index, const struct bgapi2_feature_value *value)
{
	feature_record *feature = get_feature(camera, index);
	bo_bool available = 0, writable = 0;
	int res;

	if (feature == nullptr || value == nullptr || value->kind != feature->kind)
		return -EINVAL;
	if (camera->acquiring)
		return -EBUSY;
	if ((res = checked(camera, BGAPI2_Node_GetAvailable(feature->node,
			&available))) < 0 ||
			(res = checked(camera, BGAPI2_Node_IsWriteable(feature->node,
			&writable))) < 0)
		return res;
	if (!available)
		return -ENODATA;
	if (!writable)
		return -EACCES;
	switch (feature->kind) {
	case BGAPI2_FEATURE_BOOLEAN:
		return checked(camera, BGAPI2_Node_SetBool(feature->node,
				value->boolean));
	case BGAPI2_FEATURE_INTEGER:
		return checked(camera, BGAPI2_Node_SetInt(feature->node,
				value->integer));
	case BGAPI2_FEATURE_FLOATING:
		return checked(camera, BGAPI2_Node_SetDouble(feature->node,
				value->floating));
	case BGAPI2_FEATURE_ENUMERATION:
		if (value->enumeration < 0 ||
				static_cast<size_t>(value->enumeration) >=
				feature->enum_entries.size())
			return -EINVAL;
		return checked(camera, BGAPI2_Node_SetString(feature->node,
				feature->enum_entries[value->enumeration].c_str()));
	case BGAPI2_FEATURE_STRING:
		if (value->string == nullptr)
			return -EINVAL;
		return checked(camera, BGAPI2_Node_SetString(feature->node,
				value->string));
	case BGAPI2_FEATURE_COMMAND:
		return -EINVAL;
	}
	return -EINVAL;
}

int bgapi2_camera_refresh_info(struct bgapi2_camera *camera)
{
	if (camera == nullptr)
		return -EINVAL;
	if (camera->acquiring || camera->announced_count != 0)
		return -EBUSY;
	return query_info(camera);
}

int bgapi2_camera_announce(struct bgapi2_camera *camera, void *memory,
		uint64_t size, void *user_data, BGAPI2_Buffer **buffer)
{
	int res;

	if (camera == NULL || memory == NULL || buffer == NULL ||
			size < camera->info.payload_size ||
			camera->announced_count >= SPA_IMAGE_SOURCE_MAX_BUFFERS)
		return -EINVAL;
	*buffer = NULL;
	if ((res = checked(camera, BGAPI2_CreateBufferWithExternalMemory(buffer,
			memory, size, user_data))) < 0)
		return res;
	if ((res = checked(camera, BGAPI2_DataStream_AnnounceBuffer(
			camera->stream, *buffer))) < 0) {
		BGAPI2_CALL(BGAPI2_DeleteBuffer(*buffer, NULL));
		*buffer = NULL;
		return res;
	}
	camera->announced_count++;
	return 0;
}

int bgapi2_camera_revoke(struct bgapi2_camera *camera, BGAPI2_Buffer **buffer)
{
	BGAPI2_Buffer *value;
	int res;

	if (camera == NULL || buffer == NULL || *buffer == NULL)
		return -EINVAL;
	value = *buffer;
	if ((res = checked(camera, BGAPI2_DataStream_RevokeBuffer(camera->stream,
			value, NULL))) < 0)
		return res;
	if ((res = checked(camera, BGAPI2_DeleteBuffer(value, NULL))) < 0)
		return res;
	camera->announced_count--;
	*buffer = NULL;
	return 0;
}

int bgapi2_camera_discard_buffers(struct bgapi2_camera *camera)
{
	if (camera == NULL)
		return -EINVAL;
	return checked(camera, BGAPI2_DataStream_DiscardAllBuffers(camera->stream));
}

int bgapi2_camera_start(struct bgapi2_camera *camera)
{
	int res;

	if (camera == NULL)
		return -EINVAL;
	if (camera->acquiring)
		return 0;
	spa_ringbuffer_shared_init(&camera->completion_ring);
	__atomic_store_n(&camera->completion_error, 0u, __ATOMIC_RELAXED);
	if (camera->completion_mode == BGAPI2_CAMERA_COMPLETION_CALLBACK) {
		if ((res = enable_completion_handler(camera)) < 0)
			return res;
	} else if ((res = checked(camera, BGAPI2_DataStream_SetNewBufferEventMode(
			camera->stream, EVENTMODE_POLLING))) < 0) {
		return res;
	}
	if ((res = checked(camera, BGAPI2_DataStream_StartAcquisitionContinuous(
			camera->stream))) < 0)
		goto disable_handler;
	if ((res = checked(camera, BGAPI2_Node_Execute(
			camera->acquisition_start))) < 0) {
		BGAPI2_CALL(BGAPI2_DataStream_StopAcquisition(camera->stream));
		goto disable_handler;
	}
	camera->acquiring = true;
	return 0;

disable_handler:
	(void) disable_completion_handler(camera);
	return res;
}

int bgapi2_camera_stop(struct bgapi2_camera *camera)
{
	int first_error = 0, res;

	if (camera == NULL)
		return -EINVAL;
	if (!camera->acquiring)
		return 0;
	if (camera->acquisition_abort != NULL)
		BGAPI2_CALL(BGAPI2_Node_Execute(camera->acquisition_abort));
	if ((res = checked(camera, BGAPI2_Node_Execute(
			camera->acquisition_stop))) < 0)
		first_error = res;
	if ((res = checked(camera, BGAPI2_DataStream_StopAcquisition(
			camera->stream))) < 0 && first_error == 0)
		first_error = res;
	camera->acquiring = false;
	if ((res = disable_completion_handler(camera)) < 0 && first_error == 0)
		first_error = res;
	return first_error;
}

int bgapi2_camera_queue(struct bgapi2_camera *camera, BGAPI2_Buffer *buffer)
{
	if (camera == NULL || buffer == NULL)
		return -EINVAL;
	return checked(camera, BGAPI2_DataStream_QueueBuffer(camera->stream, buffer));
}

int bgapi2_camera_try_get_completion(struct bgapi2_camera *camera,
		struct bgapi2_camera_completion *completion)
{
	BGAPI2_Buffer *buffer = nullptr;
	BGAPI2_RESULT result;
	uint32_t index;
	int32_t available;

	if (camera == NULL || completion == NULL)
		return -EINVAL;
	memset(completion, 0, sizeof(*completion));
	if (camera->completion_mode == BGAPI2_CAMERA_COMPLETION_POLLING) {
		result = BGAPI2_CALL(BGAPI2_DataStream_GetFilledBuffer(camera->stream,
				&buffer, 0));
		if (result == BGAPI2_RESULT_NO_DATA || result == BGAPI2_RESULT_TIMEOUT)
			return 0;
		if (result != BGAPI2_RESULT_SUCCESS)
			return fail(result);
		if (buffer == nullptr)
			return -EIO;
		completion->buffer = buffer;
		completion->result = read_completion(camera, buffer, completion);
		return 1;
	}
	if (__atomic_load_n(&camera->completion_error, __ATOMIC_ACQUIRE) != 0)
		return -EOVERFLOW;
	available = spa_ringbuffer_shared_get_read_index(
			&camera->completion_ring, &index);
	if (available == 0)
		return 0;
	if (available < 0 || available > (int32_t)SPA_IMAGE_SOURCE_MAX_BUFFERS)
		return -EOVERFLOW;
	*completion = camera->completions[index % SPA_IMAGE_SOURCE_MAX_BUFFERS];
	if (completion->buffer == NULL)
		return -EIO;
	spa_ringbuffer_shared_read_update(&camera->completion_ring, index + 1u);
	return 1;
}

#define GET_FRAME_FIELD(function, field) do { \
	if ((res = checked(camera, function(buffer, &info->field))) < 0) \
		return res; \
} while (0)

static int get_frame_info(struct bgapi2_camera *camera, BGAPI2_Buffer *buffer,
		struct bgapi2_frame_info *info)
{
	bo_bool incomplete = 0;
	int res;

	if (camera == NULL || buffer == NULL || info == NULL)
		return -EINVAL;
	memset(info, 0, sizeof(*info));
	info->width = camera->info.width;
	info->height = camera->info.height;
	info->offset_x = camera->info.offset_x;
	info->offset_y = camera->info.offset_y;
	GET_FRAME_FIELD(BGAPI2_Buffer_GetFrameID, frame_id);
	GET_FRAME_FIELD(BGAPI2_Buffer_GetSizeFilled, size_filled);
	if ((res = checked(camera, BGAPI2_Buffer_GetIsIncomplete(buffer,
			&incomplete))) < 0)
		return res;
	info->incomplete = incomplete != 0;
	info->image_length = info->size_filled;

	/* These layout fields are optional. Keep the negotiated layout when a
	 * producer does not implement them. */
	BGAPI2_CALL(BGAPI2_Buffer_GetXPadding(buffer, &info->x_padding));
	BGAPI2_CALL(BGAPI2_Buffer_GetImageOffset(buffer, &info->image_offset));
	BGAPI2_CALL(BGAPI2_Buffer_GetImageLength(buffer, &info->image_length));
	return 0;
}

#undef GET_FRAME_FIELD

static int read_completion(struct bgapi2_camera *camera, BGAPI2_Buffer *buffer,
		struct bgapi2_camera_completion *completion)
{
	int res;

	if ((res = checked(camera, BGAPI2_Buffer_GetUserPtr(
			buffer, &completion->user_data))) < 0)
		return res;
	return get_frame_info(camera, buffer, &completion->frame);
}
