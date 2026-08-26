/* SPDX-License-Identifier: MIT */

#include "camera.h"

#include <atcore.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct FeatureDefinition {
	const char *name;
	const wchar_t *sdk_name;
	andor3_feature_kind kind;
};

/* SDK3 does not provide feature-name enumeration. This catalog follows the
 * SDK3 2.6 feature reference; unsupported entries are discarded at open. */
constexpr FeatureDefinition feature_catalog[] = {
	{ "AccumulateCount", L"AccumulateCount", ANDOR3_FEATURE_INTEGER },
	{ "AlternatingReadoutDirection", L"AlternatingReadoutDirection", ANDOR3_FEATURE_BOOLEAN },
	{ "AOIBinning", L"AOIBinning", ANDOR3_FEATURE_ENUMERATION },
	{ "AOIHBin", L"AOIHBin", ANDOR3_FEATURE_INTEGER },
	{ "AOIHeight", L"AOIHeight", ANDOR3_FEATURE_INTEGER },
	{ "AOILayout", L"AOILayout", ANDOR3_FEATURE_ENUMERATION },
	{ "AOILeft", L"AOILeft", ANDOR3_FEATURE_INTEGER },
	{ "AOIStride", L"AOIStride", ANDOR3_FEATURE_INTEGER },
	{ "AOITop", L"AOITop", ANDOR3_FEATURE_INTEGER },
	{ "AOIVBin", L"AOIVBin", ANDOR3_FEATURE_INTEGER },
	{ "AOIWidth", L"AOIWidth", ANDOR3_FEATURE_INTEGER },
	{ "AuxiliaryOutSource", L"AuxiliaryOutSource", ANDOR3_FEATURE_ENUMERATION },
	{ "Baseline", L"Baseline", ANDOR3_FEATURE_INTEGER },
	{ "BitDepth", L"BitDepth", ANDOR3_FEATURE_ENUMERATION },
	{ "BufferOverflowEvent", L"BufferOverflowEvent", ANDOR3_FEATURE_INTEGER },
	{ "BytesPerPixel", L"BytesPerPixel", ANDOR3_FEATURE_FLOATING },
	{ "CameraAcquiring", L"CameraAcquiring", ANDOR3_FEATURE_BOOLEAN },
	{ "CameraDump", L"CameraDump", ANDOR3_FEATURE_COMMAND },
	{ "CameraModel", L"CameraModel", ANDOR3_FEATURE_STRING },
	{ "CameraName", L"CameraName", ANDOR3_FEATURE_STRING },
	{ "CameraPresent", L"CameraPresent", ANDOR3_FEATURE_BOOLEAN },
	{ "ControllerID", L"ControllerID", ANDOR3_FEATURE_STRING },
	{ "CycleMode", L"CycleMode", ANDOR3_FEATURE_ENUMERATION },
	{ "ElectronicShutteringMode", L"ElectronicShutteringMode", ANDOR3_FEATURE_ENUMERATION },
	{ "EventEnable", L"EventEnable", ANDOR3_FEATURE_BOOLEAN },
	{ "EventSelector", L"EventSelector", ANDOR3_FEATURE_ENUMERATION },
	{ "ExposureTime", L"ExposureTime", ANDOR3_FEATURE_FLOATING },
	{ "ExternalTriggerDelay", L"ExternalTriggerDelay", ANDOR3_FEATURE_FLOATING },
	{ "FanSpeed", L"FanSpeed", ANDOR3_FEATURE_ENUMERATION },
	{ "FastAOIFrameRateEnable", L"FastAOIFrameRateEnable", ANDOR3_FEATURE_BOOLEAN },
	{ "FirmwareVersion", L"FirmwareVersion", ANDOR3_FEATURE_STRING },
	{ "FrameCount", L"FrameCount", ANDOR3_FEATURE_INTEGER },
	{ "FrameRate", L"FrameRate", ANDOR3_FEATURE_FLOATING },
	{ "FullAOIControl", L"FullAOIControl", ANDOR3_FEATURE_BOOLEAN },
	{ "ImageSizeBytes", L"ImageSizeBytes", ANDOR3_FEATURE_INTEGER },
	{ "InterfaceType", L"InterfaceType", ANDOR3_FEATURE_STRING },
	{ "IOInvert", L"IOInvert", ANDOR3_FEATURE_BOOLEAN },
	{ "IOSelector", L"IOSelector", ANDOR3_FEATURE_ENUMERATION },
	{ "LineScanSpeed", L"LineScanSpeed", ANDOR3_FEATURE_FLOATING },
	{ "LUTIndex", L"LUTIndex", ANDOR3_FEATURE_INTEGER },
	{ "LUTValue", L"LUTValue", ANDOR3_FEATURE_INTEGER },
	{ "MaxInterfaceTransferRate", L"MaxInterfaceTransferRate", ANDOR3_FEATURE_FLOATING },
	{ "MetadataEnable", L"MetadataEnable", ANDOR3_FEATURE_BOOLEAN },
	{ "MetadataFrame", L"MetadataFrame", ANDOR3_FEATURE_BOOLEAN },
	{ "MetadataTimestamp", L"MetadataTimestamp", ANDOR3_FEATURE_BOOLEAN },
	{ "Overlap", L"Overlap", ANDOR3_FEATURE_BOOLEAN },
	{ "PixelEncoding", L"PixelEncoding", ANDOR3_FEATURE_ENUMERATION },
	{ "PixelHeight", L"PixelHeight", ANDOR3_FEATURE_FLOATING },
	{ "PixelReadoutRate", L"PixelReadoutRate", ANDOR3_FEATURE_ENUMERATION },
	{ "PixelWidth", L"PixelWidth", ANDOR3_FEATURE_FLOATING },
	{ "PreAmpGainControl", L"PreAmpGainControl", ANDOR3_FEATURE_ENUMERATION },
	{ "ReadoutTime", L"ReadoutTime", ANDOR3_FEATURE_FLOATING },
	{ "RollingShutterGlobalClear", L"RollingShutterGlobalClear", ANDOR3_FEATURE_BOOLEAN },
	{ "RowReadTime", L"RowReadTime", ANDOR3_FEATURE_FLOATING },
	{ "ScanSpeedControlEnable", L"ScanSpeedControlEnable", ANDOR3_FEATURE_BOOLEAN },
	{ "SensorCooling", L"SensorCooling", ANDOR3_FEATURE_BOOLEAN },
	{ "SensorHeight", L"SensorHeight", ANDOR3_FEATURE_INTEGER },
	{ "SensorModel", L"SensorModel", ANDOR3_FEATURE_STRING },
	{ "SensorReadoutMode", L"SensorReadoutMode", ANDOR3_FEATURE_ENUMERATION },
	{ "SensorTemperature", L"SensorTemperature", ANDOR3_FEATURE_FLOATING },
	{ "SensorWidth", L"SensorWidth", ANDOR3_FEATURE_INTEGER },
	{ "SerialNumber", L"SerialNumber", ANDOR3_FEATURE_STRING },
	{ "SimplePreAmpGainControl", L"SimplePreAmpGainControl", ANDOR3_FEATURE_ENUMERATION },
	{ "SoftwareTrigger", L"SoftwareTrigger", ANDOR3_FEATURE_COMMAND },
	{ "SoftwareVersion", L"SoftwareVersion", ANDOR3_FEATURE_STRING },
	{ "SpuriousNoiseFilter", L"SpuriousNoiseFilter", ANDOR3_FEATURE_BOOLEAN },
	{ "StaticBlemishCorrection", L"StaticBlemishCorrection", ANDOR3_FEATURE_BOOLEAN },
	{ "TemperatureControl", L"TemperatureControl", ANDOR3_FEATURE_ENUMERATION },
	{ "TemperatureStatus", L"TemperatureStatus", ANDOR3_FEATURE_ENUMERATION },
	{ "TimestampClock", L"TimestampClock", ANDOR3_FEATURE_INTEGER },
	{ "TimestampClockFrequency", L"TimestampClockFrequency", ANDOR3_FEATURE_INTEGER },
	{ "TimestampClockReset", L"TimestampClockReset", ANDOR3_FEATURE_COMMAND },
	{ "TriggerMode", L"TriggerMode", ANDOR3_FEATURE_ENUMERATION },
};

struct EnumEntry {
	int sdk_index;
	std::string label;
};

struct FeatureRecord {
	const FeatureDefinition *definition;
	std::string property_name;
	std::vector<EnumEntry> enum_entries;
	std::string string_value;
};

std::mutex library_mutex;
unsigned int library_users;

int fail(int result)
{
	switch (result) {
	case AT_SUCCESS: return 0;
	case AT_ERR_NOTIMPLEMENTED: return -ENOTSUP;
	case AT_ERR_READONLY:
	case AT_ERR_NOTWRITABLE: return -EACCES;
	case AT_ERR_NOTREADABLE:
	case AT_ERR_NODATA: return -ENODATA;
	case AT_ERR_OUTOFRANGE:
	case AT_ERR_INDEXNOTAVAILABLE:
	case AT_ERR_INDEXNOTIMPLEMENTED:
	case AT_ERR_INVALIDSIZE:
	case AT_ERR_INVALIDALIGNMENT: return -EINVAL;
	case AT_ERR_TIMEDOUT: return -ETIMEDOUT;
	case AT_ERR_BUFFERFULL:
	case AT_ERR_DEVICEINUSE: return -EBUSY;
	case AT_ERR_DEVICENOTFOUND: return -ENODEV;
	case AT_ERR_NOMEMORY: return -ENOMEM;
	default: return -EIO;
	}
}

int retain_library()
{
	std::lock_guard lock(library_mutex);
	if (library_users == 0) {
		const int result = AT_InitialiseLibrary();
		if (result != AT_SUCCESS)
			return fail(result);
	}
	library_users++;
	return 0;
}

void release_library()
{
	std::lock_guard lock(library_mutex);
	if (library_users != 0 && --library_users == 0)
		(void)AT_FinaliseLibrary();
}

std::string narrow(const wchar_t *value)
{
	std::string result;
	if (value == nullptr)
		return result;
	while (*value != L'\0') {
		const wchar_t character = *value++;
		result.push_back(character >= 0 && character <= 0x7f
				? static_cast<char>(character) : '?');
	}
	return result;
}

bool changes_layout(std::string_view name)
{
	static constexpr std::string_view exact[] = {
		"AOIBinning", "AOIHBin", "AOIHeight", "AOILayout", "AOILeft",
		"AOIStride", "AOITop", "AOIVBin", "AOIWidth", "ImageSizeBytes",
		"MetadataEnable", "MetadataFrame", "MetadataTimestamp", "PixelEncoding",
		"PreAmpGainControl", "SimplePreAmpGainControl",
	};
	return std::find(std::begin(exact), std::end(exact), name) != std::end(exact);
}

} // namespace

struct andor3_camera_buffer {
	AT_U8 *memory;
	int size;
	void *user_data;
	bool queued;
};

struct andor3_camera {
	AT_H handle = AT_HANDLE_UNINITIALISED;
	andor3_camera_info info{};
	std::vector<FeatureRecord> features;
	std::vector<andor3_camera_buffer *> buffers;
	std::mutex mutex;
	std::uint64_t frame_id = 0;
	bool acquiring = false;
	bool library_retained = false;
};

namespace {

int get_int(andor3_camera *camera, const wchar_t *name, AT_64 &value)
{
	return fail(AT_GetInt(camera->handle, name, &value));
}

int get_string(andor3_camera *camera, const wchar_t *name, std::string &value)
{
	int length = 0;
	int result = AT_GetStringMaxLength(camera->handle, name, &length);
	if (result != AT_SUCCESS)
		return fail(result);
	std::vector<wchar_t> buffer(static_cast<std::size_t>(std::max(length, 1)));
	result = AT_GetString(camera->handle, name, buffer.data(), buffer.size());
	if (result != AT_SUCCESS)
		return fail(result);
	value = narrow(buffer.data());
	return 0;
}

int get_enum_label(andor3_camera *camera, const wchar_t *name,
		int sdk_index, std::string &value)
{
	std::vector<wchar_t> buffer(512);
	const int result = AT_GetEnumStringByIndex(camera->handle, name, sdk_index,
			buffer.data(), buffer.size());
	if (result != AT_SUCCESS)
		return fail(result);
	value = narrow(buffer.data());
	return 0;
}

int discover_features(andor3_camera *camera)
{
	try {
		for (const auto &definition : feature_catalog) {
			AT_BOOL implemented = AT_FALSE;
			if (AT_IsImplemented(camera->handle, definition.sdk_name,
					&implemented) != AT_SUCCESS || !implemented)
				continue;
			FeatureRecord record;
			record.definition = &definition;
			record.property_name = definition.kind == ANDOR3_FEATURE_COMMAND
				? "andor3-command." : "andor3.";
			record.property_name += definition.name;
			if (definition.kind == ANDOR3_FEATURE_ENUMERATION) {
				int count = 0;
				if (AT_GetEnumCount(camera->handle, definition.sdk_name,
						&count) != AT_SUCCESS)
					continue;
				for (int i = 0; i < count; i++) {
					AT_BOOL implemented_index = AT_FALSE;
					if (AT_IsEnumIndexImplemented(camera->handle,
							definition.sdk_name, i, &implemented_index) != AT_SUCCESS ||
							!implemented_index)
						continue;
					std::string label;
					if (get_enum_label(camera, definition.sdk_name, i, label) == 0)
						record.enum_entries.push_back({ i, std::move(label) });
				}
			}
			camera->features.push_back(std::move(record));
		}
		return 0;
	} catch (const std::bad_alloc &) {
		return -ENOMEM;
	}
}

int refresh_info_locked(andor3_camera *camera)
{
	AT_64 width = 0, height = 0, stride = 0, payload = 0;
	int encoding_index = 0;
	std::string encoding;

	int result = get_int(camera, L"AOIWidth", width);
	if (result < 0) return result;
	result = get_int(camera, L"AOIHeight", height);
	if (result < 0) return result;
	result = get_int(camera, L"AOIStride", stride);
	if (result < 0) return result;
	result = get_int(camera, L"ImageSizeBytes", payload);
	if (result < 0) return result;
	if (width <= 0 || height <= 0 || stride <= 0 || payload <= 0 ||
			width > UINT32_MAX || height > UINT32_MAX || stride > UINT32_MAX ||
			payload > INT_MAX || height > payload / stride)
		return -ERANGE;
	result = fail(AT_GetEnumIndex(camera->handle, L"PixelEncoding",
			&encoding_index));
	if (result < 0) return result;
	result = get_enum_label(camera, L"PixelEncoding", encoding_index, encoding);
	if (result < 0) return result;
	camera->info.width = static_cast<std::uint32_t>(width);
	camera->info.height = static_cast<std::uint32_t>(height);
	camera->info.stride = static_cast<std::uint32_t>(stride);
	camera->info.image_size = static_cast<std::uint64_t>(stride * height);
	camera->info.payload_size = static_cast<std::uint64_t>(payload);
	std::snprintf(camera->info.pixel_encoding,
			sizeof(camera->info.pixel_encoding), "%s", encoding.c_str());
	std::string text;
	if (get_string(camera, L"CameraModel", text) == 0)
		std::snprintf(camera->info.model, sizeof(camera->info.model), "%s",
				text.c_str());
	else
		std::snprintf(camera->info.model, sizeof(camera->info.model),
				"Andor SDK3 camera");
	if (get_string(camera, L"SerialNumber", text) == 0)
		std::snprintf(camera->info.serial, sizeof(camera->info.serial), "%s",
				text.c_str());
	else
		camera->info.serial[0] = '\0';
	return 0;
}

FeatureRecord *feature(andor3_camera *camera, std::uint32_t index)
{
	return index < camera->features.size() ? &camera->features[index] : nullptr;
}

} // namespace

extern "C" int andor3_camera_open(struct andor3_camera **camera_ptr,
		const struct andor3_camera_options *options)
{
	if (camera_ptr == nullptr || options == nullptr ||
			options->device_index > INT_MAX)
		return -EINVAL;
	*camera_ptr = nullptr;
	auto camera = std::unique_ptr<andor3_camera>(
			new (std::nothrow) andor3_camera);
	if (!camera)
		return -ENOMEM;
	int result = retain_library();
	if (result < 0)
		return result;
	camera->library_retained = true;
	AT_64 count = 0;
	if ((result = fail(AT_GetInt(AT_HANDLE_SYSTEM, L"DeviceCount", &count))) < 0 ||
			options->device_index >= static_cast<std::uint64_t>(count)) {
		release_library();
		return result < 0 ? result : -ENODEV;
	}
	result = fail(AT_Open(static_cast<int>(options->device_index),
			&camera->handle));
	if (result < 0) {
		release_library();
		return result;
	}
	if ((result = discover_features(camera.get())) < 0 ||
			(result = refresh_info_locked(camera.get())) < 0) {
		(void)AT_Close(camera->handle);
		release_library();
		return result;
	}
	*camera_ptr = camera.release();
	return 0;
}

extern "C" void andor3_camera_close(struct andor3_camera *camera)
{
	if (camera == nullptr)
		return;
	(void)andor3_camera_stop(camera);
	for (auto *buffer : camera->buffers)
		delete buffer;
	if (camera->handle != AT_HANDLE_UNINITIALISED)
		(void)AT_Close(camera->handle);
	if (camera->library_retained)
		release_library();
	delete camera;
}

extern "C" const struct andor3_camera_info *andor3_camera_get_info(
		const struct andor3_camera *camera)
{
	return camera == nullptr ? nullptr : &camera->info;
}

extern "C" uint32_t andor3_camera_get_feature_count(
		const struct andor3_camera *camera)
{
	return camera == nullptr ? 0 : static_cast<std::uint32_t>(camera->features.size());
}

extern "C" int andor3_camera_get_feature_info(struct andor3_camera *camera,
		uint32_t index, struct andor3_feature_info *info)
{
	if (camera == nullptr || info == nullptr)
		return -EINVAL;
	std::lock_guard lock(camera->mutex);
	auto *record = feature(camera, index);
	if (record == nullptr)
		return -ENOENT;
	AT_BOOL readable = AT_FALSE, writable = AT_FALSE;
	(void)AT_IsReadable(camera->handle, record->definition->sdk_name, &readable);
	(void)AT_IsWritable(camera->handle, record->definition->sdk_name, &writable);
	*info = {
		.name = record->definition->name,
		.property_name = record->property_name.c_str(),
		.description = record->definition->name,
		.kind = record->definition->kind,
		.n_enum_entries = static_cast<std::uint32_t>(record->enum_entries.size()),
		.available = readable || writable,
		.readable = readable != AT_FALSE,
		.writable = writable != AT_FALSE,
		.changes_layout = changes_layout(record->definition->name),
	};
	return 0;
}

extern "C" const char *andor3_camera_get_feature_enum_entry(
		const struct andor3_camera *camera, uint32_t index, uint32_t entry_index)
{
	if (camera == nullptr || index >= camera->features.size() ||
			entry_index >= camera->features[index].enum_entries.size())
		return nullptr;
	return camera->features[index].enum_entries[entry_index].label.c_str();
}

extern "C" int andor3_camera_get_feature_value(struct andor3_camera *camera,
		uint32_t index, struct andor3_feature_value *value)
{
	if (camera == nullptr || value == nullptr)
		return -EINVAL;
	std::lock_guard lock(camera->mutex);
	auto *record = feature(camera, index);
	if (record == nullptr)
		return -ENOENT;
	value->kind = record->definition->kind;
	int result;
	switch (record->definition->kind) {
	case ANDOR3_FEATURE_BOOLEAN: {
		AT_BOOL item = AT_FALSE;
		result = AT_GetBool(camera->handle, record->definition->sdk_name, &item);
		value->boolean = item != AT_FALSE;
		break;
	}
	case ANDOR3_FEATURE_INTEGER: {
		AT_64 item = 0;
		result = AT_GetInt(camera->handle, record->definition->sdk_name, &item);
		value->integer = item;
		break;
	}
	case ANDOR3_FEATURE_FLOATING:
		result = AT_GetFloat(camera->handle, record->definition->sdk_name,
				&value->floating);
		break;
	case ANDOR3_FEATURE_ENUMERATION: {
		int sdk_index = 0;
		result = AT_GetEnumIndex(camera->handle, record->definition->sdk_name,
				&sdk_index);
		if (result == AT_SUCCESS) {
			const auto found = std::find_if(record->enum_entries.begin(),
					record->enum_entries.end(), [sdk_index](const EnumEntry &entry) {
						return entry.sdk_index == sdk_index;
					});
			if (found == record->enum_entries.end())
				return -ENODATA;
			value->enumeration = static_cast<std::int32_t>(
					std::distance(record->enum_entries.begin(), found));
		}
		break;
	}
	case ANDOR3_FEATURE_STRING:
		if ((result = get_string(camera, record->definition->sdk_name,
				record->string_value)) == 0) {
			value->string = record->string_value.c_str();
			return 0;
		}
		return result;
	case ANDOR3_FEATURE_COMMAND:
		return -ENODATA;
	default:
		return -EINVAL;
	}
	return fail(result);
}

extern "C" int andor3_camera_get_feature_integer_range(
		struct andor3_camera *camera, uint32_t index,
		int64_t *minimum, int64_t *maximum)
{
	if (camera == nullptr || minimum == nullptr || maximum == nullptr)
		return -EINVAL;
	std::lock_guard lock(camera->mutex);
	auto *record = feature(camera, index);
	if (record == nullptr || record->definition->kind != ANDOR3_FEATURE_INTEGER)
		return -EINVAL;
	AT_64 low = 0, high = 0;
	int result = AT_GetIntMin(camera->handle, record->definition->sdk_name, &low);
	if (result == AT_SUCCESS)
		result = AT_GetIntMax(camera->handle, record->definition->sdk_name, &high);
	if (result != AT_SUCCESS)
		return fail(result);
	*minimum = low;
	*maximum = high;
	return 0;
}

extern "C" int andor3_camera_get_feature_float_range(
		struct andor3_camera *camera, uint32_t index,
		double *minimum, double *maximum)
{
	if (camera == nullptr || minimum == nullptr || maximum == nullptr)
		return -EINVAL;
	std::lock_guard lock(camera->mutex);
	auto *record = feature(camera, index);
	if (record == nullptr || record->definition->kind != ANDOR3_FEATURE_FLOATING)
		return -EINVAL;
	int result = AT_GetFloatMin(camera->handle, record->definition->sdk_name,
			minimum);
	if (result == AT_SUCCESS)
		result = AT_GetFloatMax(camera->handle, record->definition->sdk_name,
				maximum);
	return fail(result);
}

extern "C" int andor3_camera_find_feature(const struct andor3_camera *camera,
		const char *property_name, uint32_t *index)
{
	if (camera == nullptr || property_name == nullptr || index == nullptr)
		return -EINVAL;
	const auto found = std::find_if(camera->features.begin(), camera->features.end(),
			[property_name](const FeatureRecord &record) {
				return record.property_name == property_name;
			});
	if (found == camera->features.end())
		return -ENOENT;
	*index = static_cast<std::uint32_t>(
			std::distance(camera->features.begin(), found));
	return 0;
}

extern "C" int andor3_camera_set_feature_value(struct andor3_camera *camera,
		uint32_t index, const struct andor3_feature_value *value)
{
	if (camera == nullptr || value == nullptr)
		return -EINVAL;
	std::lock_guard lock(camera->mutex);
	auto *record = feature(camera, index);
	if (record == nullptr || value->kind != record->definition->kind)
		return -EINVAL;
	int result;
	switch (record->definition->kind) {
	case ANDOR3_FEATURE_BOOLEAN:
		result = AT_SetBool(camera->handle, record->definition->sdk_name,
				value->boolean ? AT_TRUE : AT_FALSE);
		break;
	case ANDOR3_FEATURE_INTEGER:
		result = AT_SetInt(camera->handle, record->definition->sdk_name,
				value->integer);
		break;
	case ANDOR3_FEATURE_FLOATING:
		result = AT_SetFloat(camera->handle, record->definition->sdk_name,
				value->floating);
		break;
	case ANDOR3_FEATURE_ENUMERATION:
		if (value->enumeration < 0 ||
				static_cast<std::size_t>(value->enumeration) >=
				record->enum_entries.size())
			return -EINVAL;
		result = AT_SetEnumIndex(camera->handle, record->definition->sdk_name,
				record->enum_entries[value->enumeration].sdk_index);
		break;
	case ANDOR3_FEATURE_STRING: {
		if (value->string == nullptr)
			return -EINVAL;
		std::wstring wide;
		try {
			const char *text = value->string;
			while (*text != '\0')
				wide.push_back(static_cast<unsigned char>(*text++));
		} catch (const std::bad_alloc &) {
			return -ENOMEM;
		}
		result = AT_SetString(camera->handle, record->definition->sdk_name,
				wide.c_str());
		break;
	}
	case ANDOR3_FEATURE_COMMAND:
		if (!value->boolean)
			return 0;
		result = AT_Command(camera->handle, record->definition->sdk_name);
		break;
	default:
		return -EINVAL;
	}
	return fail(result);
}

extern "C" int andor3_camera_refresh_info(struct andor3_camera *camera)
{
	if (camera == nullptr)
		return -EINVAL;
	std::lock_guard lock(camera->mutex);
	if (camera->acquiring || !camera->buffers.empty())
		return -EBUSY;
	return refresh_info_locked(camera);
}

extern "C" int andor3_camera_announce(struct andor3_camera *camera,
		void *memory, uint64_t size, void *user_data,
		struct andor3_camera_buffer **buffer_ptr)
{
	if (camera == nullptr || memory == nullptr || buffer_ptr == nullptr ||
			size < camera->info.payload_size || camera->info.payload_size > INT_MAX ||
			(reinterpret_cast<std::uintptr_t>(memory) & 7u) != 0)
		return -EINVAL;
	std::lock_guard lock(camera->mutex);
	if (camera->acquiring || camera->buffers.size() >= ANDOR3_CAMERA_MAX_BUFFERS)
		return -EBUSY;
	auto buffer = std::unique_ptr<andor3_camera_buffer>(
			new (std::nothrow) andor3_camera_buffer);
	if (!buffer)
		return -ENOMEM;
	buffer->memory = static_cast<AT_U8 *>(memory);
	buffer->size = static_cast<int>(camera->info.payload_size);
	buffer->user_data = user_data;
	buffer->queued = false;
	try {
		camera->buffers.push_back(buffer.get());
	} catch (const std::bad_alloc &) {
		return -ENOMEM;
	}
	*buffer_ptr = buffer.release();
	return 0;
}

extern "C" int andor3_camera_revoke(struct andor3_camera *camera,
		struct andor3_camera_buffer **buffer_ptr)
{
	if (camera == nullptr || buffer_ptr == nullptr || *buffer_ptr == nullptr)
		return -EINVAL;
	std::lock_guard lock(camera->mutex);
	if (camera->acquiring || (*buffer_ptr)->queued)
		return -EBUSY;
	const auto found = std::find(camera->buffers.begin(), camera->buffers.end(),
			*buffer_ptr);
	if (found == camera->buffers.end())
		return -ENOENT;
	delete *found;
	camera->buffers.erase(found);
	*buffer_ptr = nullptr;
	return 0;
}

extern "C" int andor3_camera_queue(struct andor3_camera *camera,
		struct andor3_camera_buffer *buffer)
{
	if (camera == nullptr || buffer == nullptr)
		return -EINVAL;
	std::lock_guard lock(camera->mutex);
	if (buffer->queued || std::find(camera->buffers.begin(), camera->buffers.end(),
			buffer) == camera->buffers.end())
		return -EINVAL;
	const int result = AT_QueueBuffer(camera->handle, buffer->memory, buffer->size);
	if (result != AT_SUCCESS)
		return fail(result);
	buffer->queued = true;
	return 0;
}

extern "C" int andor3_camera_start(struct andor3_camera *camera)
{
	if (camera == nullptr)
		return -EINVAL;
	std::lock_guard lock(camera->mutex);
	if (camera->acquiring)
		return 0;
	if (camera->buffers.size() < 2)
		return -EINVAL;
	const int result = AT_Command(camera->handle, L"AcquisitionStart");
	if (result != AT_SUCCESS)
		return fail(result);
	camera->frame_id = 0;
	camera->acquiring = true;
	return 0;
}

extern "C" int andor3_camera_stop(struct andor3_camera *camera)
{
	if (camera == nullptr)
		return -EINVAL;
	std::lock_guard lock(camera->mutex);
	int first_error = 0;
	if (camera->acquiring) {
		const int result = AT_Command(camera->handle, L"AcquisitionStop");
		if (result != AT_SUCCESS)
			first_error = fail(result);
	}
	const int flush_result = AT_Flush(camera->handle);
	if (flush_result != AT_SUCCESS && first_error == 0)
		first_error = fail(flush_result);
	for (auto *buffer : camera->buffers)
		buffer->queued = false;
	camera->acquiring = false;
	return first_error;
}

extern "C" int andor3_camera_try_get_completion(struct andor3_camera *camera,
		struct andor3_camera_completion *completion)
{
	if (camera == nullptr || completion == nullptr)
		return -EINVAL;
	std::lock_guard lock(camera->mutex);
	if (!camera->acquiring)
		return -EINVAL;
	AT_U8 *memory = nullptr;
	int size = 0;
	const int result = AT_WaitBuffer(camera->handle, &memory, &size, 0);
	if (result == AT_ERR_TIMEDOUT || result == AT_ERR_NODATA)
		return 0;
	if (result != AT_SUCCESS)
		return fail(result);
	const auto found = std::find_if(camera->buffers.begin(), camera->buffers.end(),
			[memory](const andor3_camera_buffer *buffer) {
				return buffer->memory == memory && buffer->queued;
			});
	if (found == camera->buffers.end())
		return -EPROTO;
	(*found)->queued = false;
	*completion = {
		.buffer = *found,
		.user_data = (*found)->user_data,
		.frame_id = ++camera->frame_id,
		.size_filled = camera->info.image_size,
		.incomplete = size < static_cast<int>(camera->info.image_size),
	};
	return 1;
}
