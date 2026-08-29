/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <dcamapi4.h>
#include <dcamprop.h>

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DCAM_RING_FRAMES 16u
#define PROPERTY_NAME_CAPACITY 192u
#define FEATURE_NAME_CAPACITY 128u
#define DESCRIPTION_CAPACITY 320u
#define VALUE_TEXT_CAPACITY 128u

struct hamamatsu_camera_buffer {
	void *memory;
	uint64_t size;
	void *user_data;
	bool queued;
};

struct enum_entry {
	double value;
	char *label;
};

struct dcam_feature {
	int32 id;
	char name[FEATURE_NAME_CAPACITY];
	char property_name[PROPERTY_NAME_CAPACITY];
	char description[DESCRIPTION_CAPACITY];
	enum hamamatsu_feature_kind kind;
	struct enum_entry *entries;
	uint32_t n_entries;
	bool changes_layout;
};

struct hamamatsu_camera {
	HDCAM handle;
	bool api_acquired;
	struct hamamatsu_camera_info info;
	struct dcam_feature *features;
	uint32_t n_features;
	struct hamamatsu_camera_buffer *buffers[HAMAMATSU_CAMERA_MAX_BUFFERS];
	uint32_t n_buffers;
	uint32_t scan_hint;
	bool capture_buffers_allocated;
	int32_t last_frame_count;
	bool started;
};

static pthread_mutex_t api_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t api_users;

static bool dcam_succeeded(DCAMERR error)
{
	return (int32_t)error >= 0;
}

static int dcam_error(DCAMERR error)
{
	switch ((int32_t)error) {
	case (int32_t)DCAMERR_INVALIDPARAM: return -EINVAL;
	case (int32_t)DCAMERR_INVALIDVALUE:
	case (int32_t)DCAMERR_OUTOFRANGE: return -ERANGE;
	case (int32_t)DCAMERR_NOTWRITABLE: return -EACCES;
	case (int32_t)DCAMERR_NOTREADABLE: return -ENODATA;
	case (int32_t)DCAMERR_INVALIDPROPERTYID:
	case (int32_t)DCAMERR_NOPROPERTY: return -ENOENT;
	case (int32_t)DCAMERR_ACCESSDENY: return -EBUSY;
	case (int32_t)DCAMERR_BUSY:
	case (int32_t)DCAMERR_NOTREADY:
	case (int32_t)DCAMERR_NOTSTABLE:
	case (int32_t)DCAMERR_UNSTABLE:
	case (int32_t)DCAMERR_NOTBUSY:
	case (int32_t)DCAMERR_EXCLUDED: return -EBUSY;
	case (int32_t)DCAMERR_ABORT: return -ECANCELED;
	case (int32_t)DCAMERR_NOMEMORY: return -ENOMEM;
	case (int32_t)DCAMERR_NORESOURCE: return -ENOSPC;
	case (int32_t)DCAMERR_NOCONNECTION:
	case (int32_t)DCAMERR_INVALIDCAMERA:
	case (int32_t)DCAMERR_NOCAMERA:
	case (int32_t)DCAMERR_NOGRABBER:
	case (int32_t)DCAMERR_NODRIVER:
	case (int32_t)DCAMERR_NOMODULE: return -ENODEV;
	case (int32_t)DCAMERR_TIMEOUT: return -EAGAIN;
	case (int32_t)DCAMERR_NOTSUPPORT:
	case (int32_t)DCAMERR_NOTIMPLEMENT: return -ENOTSUP;
	default: return -EIO;
	}
}

static int api_acquire(uint32_t requested_index)
{
	DCAMAPI_INIT init = { .size = sizeof(init) };
	DCAMERR error;
	int result = 0;

	pthread_mutex_lock(&api_lock);
	if (api_users == 0) {
		error = dcamapi_init(&init);
		if (!dcam_succeeded(error)) {
			result = dcam_error(error);
			goto done;
		}
	} else {
		/* DCAM-API does not offer a device-count query after initialization. */
		init.iDeviceCount = INT32_MAX;
	}
	if (requested_index >= (uint32_t)init.iDeviceCount) {
		if (api_users == 0)
			dcamapi_uninit();
		result = -ENODEV;
		goto done;
	}
	api_users++;
done:
	pthread_mutex_unlock(&api_lock);
	return result;
}

static void api_release(void)
{
	pthread_mutex_lock(&api_lock);
	if (api_users > 0 && --api_users == 0)
		dcamapi_uninit();
	pthread_mutex_unlock(&api_lock);
}

static int get_property(struct hamamatsu_camera *camera, int32 id, double *value)
{
	DCAMERR error = dcamprop_getvalue(camera->handle, id, value);
	return dcam_succeeded(error) ? 0 : dcam_error(error);
}

static int get_string(HDCAM handle, int32 id, char *text, size_t capacity)
{
	DCAMDEV_STRING string = {
		.size = sizeof(string), .iString = id, .text = text,
		.textbytes = (int32)capacity,
	};
	DCAMERR error;

	if (capacity == 0 || capacity > INT32_MAX)
		return -EINVAL;
	text[0] = '\0';
	text[capacity - 1u] = '\0';
	error = dcamdev_getstring(handle, &string);
	return dcam_succeeded(error) ? 0 : dcam_error(error);
}

static const char *unit_name(int32 unit)
{
	switch (unit) {
	case DCAMPROP_UNIT_SECOND: return "s";
	case DCAMPROP_UNIT_CELSIUS: return "deg C";
	case DCAMPROP_UNIT_KELVIN: return "K";
	case DCAMPROP_UNIT_METERPERSECOND: return "m/s";
	case DCAMPROP_UNIT_PERSECOND: return "Hz";
	case DCAMPROP_UNIT_DEGREE: return "deg";
	case DCAMPROP_UNIT_MICROMETER: return "um";
	default: return NULL;
	}
}

static const char *sfnc_alias(int32 id)
{
	switch (id) {
	case DCAM_IDPROP_EXPOSURETIME: return "ExposureTime";
	case DCAM_IDPROP_SUBARRAYHPOS: return "OffsetX";
	case DCAM_IDPROP_SUBARRAYHSIZE: return "Width";
	case DCAM_IDPROP_SUBARRAYVPOS: return "OffsetY";
	case DCAM_IDPROP_SUBARRAYVSIZE: return "Height";
	case DCAM_IDPROP_IMAGE_PIXELTYPE: return "PixelFormat";
	case DCAM_IDPROP_TRIGGERSOURCE: return "TriggerSource";
	case DCAM_IDPROP_SENSORTEMPERATURE: return "DeviceTemperature";
	case DCAM_IDPROP_SENSORCOOLER: return "SensorCooling";
	default: return NULL;
	}
}

static void make_feature_name(char *destination, size_t capacity,
		const char *vendor_name)
{
	size_t output = 0;
	bool capitalize = true;
	const unsigned char *input = (const unsigned char *)vendor_name;

	while (*input != '\0' && output + 1u < capacity) {
		unsigned char c = *input++;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9')) {
			if (capitalize && c >= 'a' && c <= 'z')
				c = (unsigned char)(c - 'a' + 'A');
			destination[output++] = (char)c;
			capitalize = false;
		} else {
			capitalize = true;
		}
	}
	destination[output] = '\0';
}

static bool changes_layout(int32 id)
{
	switch (id) {
	case DCAM_IDPROP_SENSORMODE:
	case DCAM_IDPROP_READOUTSPEED:
	case DCAM_IDPROP_OUTPUTDATA_OPERATION:
	case DCAM_IDPROP_BINNING:
	case DCAM_IDPROP_BINNING_INDEPENDENT:
	case DCAM_IDPROP_BINNING_HORZ:
	case DCAM_IDPROP_BINNING_VERT:
	case DCAM_IDPROP_SUBARRAYHPOS:
	case DCAM_IDPROP_SUBARRAYHSIZE:
	case DCAM_IDPROP_SUBARRAYVPOS:
	case DCAM_IDPROP_SUBARRAYVSIZE:
	case DCAM_IDPROP_SUBARRAYMODE:
	case DCAM_IDPROP_DIGITALBINNING_METHOD:
	case DCAM_IDPROP_DIGITALBINNING_HORZ:
	case DCAM_IDPROP_DIGITALBINNING_VERT:
	case DCAM_IDPROP_COLORTYPE:
	case DCAM_IDPROP_BITSPERCHANNEL:
	case DCAM_IDPROP_IMAGE_PIXELTYPE:
	case DCAM_IDPROP_FRAMEBUNDLE_MODE:
	case DCAM_IDPROP_FRAMEBUNDLE_NUMBER:
		return true;
	default:
		return false;
	}
}

static void free_feature(struct dcam_feature *feature)
{
	uint32_t i;

	for (i = 0; i < feature->n_entries; i++)
		free(feature->entries[i].label);
	free(feature->entries);
}

static const char *pixel_type_name(double value)
{
	switch ((int32_t)llround(value)) {
	case DCAM_PIXELTYPE_MONO8: return "Mono8";
	case DCAM_PIXELTYPE_MONO12: return "Mono12";
	case DCAM_PIXELTYPE_MONO12P: return "Mono12Packed";
	case DCAM_PIXELTYPE_MONO16: return "Mono16";
	default: return NULL;
	}
}

static int enumerate_mode_values(struct hamamatsu_camera *camera,
		struct dcam_feature *feature, const DCAMPROP_ATTR *attribute)
{
	double value = attribute->valuemin;
	uint32_t count = 0;

	for (;;) {
		DCAMPROP_VALUETEXT value_text;
		struct enum_entry *entries;
		char text[VALUE_TEXT_CAPACITY];
		DCAMERR error;

		memset(&value_text, 0, sizeof(value_text));
		value_text.cbSize = sizeof(value_text);
		value_text.iProp = feature->id;
		value_text.value = value;
		value_text.text = text;
		value_text.textbytes = sizeof(text);
		text[0] = '\0';
		text[sizeof(text) - 1u] = '\0';
		error = dcamprop_getvaluetext(camera->handle, &value_text);
		if (!dcam_succeeded(error))
			break;
		entries = realloc(feature->entries, (count + 1u) * sizeof(*entries));
		if (entries == NULL)
			return -errno;
		feature->entries = entries;
		feature->entries[count].value = value;
		feature->entries[count].label = strdup(feature->id ==
				DCAM_IDPROP_IMAGE_PIXELTYPE && pixel_type_name(value) != NULL ?
				pixel_type_name(value) : text);
		if (feature->entries[count].label == NULL)
			return -errno;
		count++;
		feature->n_entries = count;
		if (count >= 4096u)
			return -E2BIG;
		if (value >= attribute->valuemax)
			break;
		{
			double previous = value;

			error = dcamprop_queryvalue(camera->handle, feature->id, &value,
					DCAMPROP_OPTION_NEXT);
			if (!dcam_succeeded(error))
				break;
			if (value == previous)
				return -EPROTO;
		}
	}
	return 0;
}

static int add_property_feature(struct hamamatsu_camera *camera, int32 id)
{
	DCAMPROP_ATTR attribute = { .cbSize = sizeof(attribute), .iProp = id };
	struct dcam_feature *features, *feature;
	const char *alias, *unit;
	char vendor_name[FEATURE_NAME_CAPACITY];
	DCAMERR error;
	int result;

	error = dcamprop_getattr(camera->handle, &attribute);
	if (!dcam_succeeded(error))
		return dcam_error(error);
	if ((attribute.attribute2 & DCAMPROP_ATTR2_ARRAYELEMENT) != 0)
		return 0;
	error = dcamprop_getname(camera->handle, id, vendor_name, sizeof(vendor_name));
	if (!dcam_succeeded(error))
		return dcam_error(error);
	vendor_name[sizeof(vendor_name) - 1u] = '\0';
	features = realloc(camera->features,
			(camera->n_features + 1u) * sizeof(*features));
	if (features == NULL)
		return -errno;
	camera->features = features;
	feature = &features[camera->n_features];
	memset(feature, 0, sizeof(*feature));
	feature->id = id;
	alias = sfnc_alias(id);
	if (alias != NULL)
		snprintf(feature->name, sizeof(feature->name), "%s", alias);
	else
		make_feature_name(feature->name, sizeof(feature->name), vendor_name);
	if (feature->name[0] == '\0')
		snprintf(feature->name, sizeof(feature->name), "Dcam%08x",
				(unsigned int)id);
	{
		uint32_t i;
		for (i = 0; i < camera->n_features; i++) {
			if (strcmp(camera->features[i].name, feature->name) == 0) {
				size_t used = strlen(feature->name);
				snprintf(feature->name + used, sizeof(feature->name) - used,
						"_%08x", (unsigned int)id);
				break;
			}
		}
	}
	snprintf(feature->property_name, sizeof(feature->property_name),
			"hamamatsu.%s", feature->name);
	unit = unit_name(attribute.iUnit);
	if (unit != NULL)
		snprintf(feature->description, sizeof(feature->description),
				"%s (DCAM-API property 0x%08x, unit: %s)", vendor_name,
				(unsigned int)id, unit);
	else
		snprintf(feature->description, sizeof(feature->description),
				"%s (DCAM-API property 0x%08x)", vendor_name,
				(unsigned int)id);
	feature->changes_layout = changes_layout(id);
	switch (attribute.attribute & DCAMPROP_TYPE_MASK) {
	case DCAMPROP_TYPE_MODE:
		feature->kind = HAMAMATSU_FEATURE_ENUMERATION;
		result = enumerate_mode_values(camera, feature, &attribute);
		if (result < 0) {
			free_feature(feature);
			return result;
		}
		break;
	case DCAMPROP_TYPE_LONG:
		feature->kind = HAMAMATSU_FEATURE_INTEGER;
		break;
	case DCAMPROP_TYPE_REAL:
		feature->kind = HAMAMATSU_FEATURE_FLOATING;
		break;
	default:
		return 0;
	}
	camera->n_features++;
	return 0;
}

static int enumerate_features(struct hamamatsu_camera *camera)
{
	int32 id = 0;

	for (;;) {
		DCAMERR error = dcamprop_getnextid(camera->handle, &id,
				DCAMPROP_OPTION_SUPPORT);
		int result;

		if (!dcam_succeeded(error))
			return error == DCAMERR_NOPROPERTY ? 0 : dcam_error(error);
		result = add_property_feature(camera, id);
		if (result < 0)
			return result;
	}
}

static int refresh_info(struct hamamatsu_camera *camera)
{
	double width, height, stride, frame_bytes, pixel_type, frame_bundle;
	int result;

	result = get_property(camera, DCAM_IDPROP_FRAMEBUNDLE_MODE, &frame_bundle);
	if (result == 0 && (int32_t)llround(frame_bundle) != DCAMPROP_MODE__OFF)
		return -ENOTSUP;
	if (result < 0 && result != -ENOENT && result != -ENOTSUP &&
			result != -ENODATA)
		return result;
	if ((result = get_property(camera, DCAM_IDPROP_IMAGE_WIDTH, &width)) < 0 ||
			(result = get_property(camera, DCAM_IDPROP_IMAGE_HEIGHT, &height)) < 0 ||
			(result = get_property(camera, DCAM_IDPROP_BUFFER_ROWBYTES, &stride)) < 0 ||
			(result = get_property(camera, DCAM_IDPROP_BUFFER_FRAMEBYTES,
				&frame_bytes)) < 0 ||
			(result = get_property(camera, DCAM_IDPROP_BUFFER_PIXELTYPE,
				&pixel_type)) < 0)
		return result;
	if (width < 1 || width > UINT32_MAX || height < 1 || height > UINT32_MAX ||
			stride < 1 || stride > UINT32_MAX || frame_bytes < 1 ||
			frame_bytes > INT32_MAX)
		return -ERANGE;
	camera->info.width = (uint32_t)llround(width);
	camera->info.height = (uint32_t)llround(height);
	camera->info.stride = (uint32_t)llround(stride);
	camera->info.image_size = (uint64_t)llround(frame_bytes);
	camera->info.payload_size = camera->info.image_size;
	switch ((int32_t)llround(pixel_type)) {
	case DCAM_PIXELTYPE_MONO8: strcpy(camera->info.pixel_encoding, "Mono8"); break;
	case DCAM_PIXELTYPE_MONO12: strcpy(camera->info.pixel_encoding, "Mono12"); break;
	case DCAM_PIXELTYPE_MONO12P: strcpy(camera->info.pixel_encoding, "Mono12Packed"); break;
	case DCAM_PIXELTYPE_MONO16: strcpy(camera->info.pixel_encoding, "Mono16"); break;
	default: snprintf(camera->info.pixel_encoding,
				sizeof(camera->info.pixel_encoding), "DCAM-0x%08x",
				(unsigned int)llround(pixel_type)); break;
	}
	return 0;
}

int hamamatsu_camera_open(struct hamamatsu_camera **camera_ptr,
		const struct hamamatsu_camera_options *options)
{
	DCAMDEV_OPEN open;
	struct hamamatsu_camera *camera;
	DCAMERR error;
	int result;

	if (camera_ptr == NULL || options == NULL)
		return -EINVAL;
	*camera_ptr = NULL;
	if ((result = api_acquire(options->device_index)) < 0)
		return result;
	camera = calloc(1, sizeof(*camera));
	if (camera == NULL) {
		api_release();
		return -errno;
	}
	camera->api_acquired = true;
	memset(&open, 0, sizeof(open));
	open.size = sizeof(open);
	open.index = (int32)options->device_index;
	error = dcamdev_open(&open);
	if (!dcam_succeeded(error)) {
		result = dcam_error(error);
		goto error;
	}
	camera->handle = open.hdcam;
	(void)get_string(camera->handle, DCAM_IDSTR_MODEL, camera->info.model,
			sizeof(camera->info.model));
	(void)get_string(camera->handle, DCAM_IDSTR_CAMERAID, camera->info.serial,
			sizeof(camera->info.serial));
	if (camera->info.model[0] == '\0')
		strcpy(camera->info.model, "Hamamatsu DCAM camera");
	if ((result = refresh_info(camera)) < 0 ||
			(result = enumerate_features(camera)) < 0)
		goto error;
	*camera_ptr = camera;
	return 0;

error:
	hamamatsu_camera_close(camera);
	return result;
}

void hamamatsu_camera_close(struct hamamatsu_camera *camera)
{
	uint32_t i;

	if (camera == NULL)
		return;
	(void)hamamatsu_camera_stop(camera);
	for (i = 0; i < camera->n_buffers; i++)
		free(camera->buffers[i]);
	for (i = 0; i < camera->n_features; i++)
		free_feature(&camera->features[i]);
	free(camera->features);
	if (camera->handle != NULL)
		dcamdev_close(camera->handle);
	if (camera->api_acquired)
		api_release();
	free(camera);
}

const struct hamamatsu_camera_info *hamamatsu_camera_get_info(
		const struct hamamatsu_camera *camera)
{
	return camera == NULL ? NULL : &camera->info;
}

uint32_t hamamatsu_camera_get_feature_count(const struct hamamatsu_camera *camera)
{
	return camera == NULL ? 0 : camera->n_features + 1u;
}

static int current_attribute(struct hamamatsu_camera *camera,
		const struct dcam_feature *feature, DCAMPROP_ATTR *attribute)
{
	DCAMERR error;

	memset(attribute, 0, sizeof(*attribute));
	attribute->cbSize = sizeof(*attribute);
	attribute->iProp = feature->id;
	error = dcamprop_getattr(camera->handle, attribute);
	return dcam_succeeded(error) ? 0 : dcam_error(error);
}

int hamamatsu_camera_get_feature_info(struct hamamatsu_camera *camera,
		uint32_t index, struct hamamatsu_feature_info *info)
{
	struct dcam_feature *feature;
	DCAMPROP_ATTR attribute;
	bool ready;
	int status = DCAMCAP_STATUS_READY;

	if (camera == NULL || info == NULL || index > camera->n_features)
		return -EINVAL;
	if (index == camera->n_features) {
		*info = (struct hamamatsu_feature_info) {
			.name = "SoftwareTrigger",
			.property_name = "hamamatsu-command.SoftwareTrigger",
			.description = "Generate a DCAM software trigger while acquisition is running",
			.kind = HAMAMATSU_FEATURE_COMMAND,
			.available = camera->started,
			.writable = camera->started,
		};
		return 0;
	}
	feature = &camera->features[index];
	if (current_attribute(camera, feature, &attribute) < 0) {
		*info = (struct hamamatsu_feature_info) {
			.name = feature->name, .property_name = feature->property_name,
			.description = feature->description, .kind = feature->kind,
			.n_enum_entries = feature->n_entries,
			.changes_layout = feature->changes_layout,
		};
		return 0;
	}
	if (camera->started)
		(void)dcamcap_status(camera->handle, &status);
	ready = status != DCAMCAP_STATUS_BUSY;
	{
		const int32 access = attribute.attribute &
				(DCAMPROP_ATTR_ACCESSREADY | DCAMPROP_ATTR_ACCESSBUSY);
		const bool state_access = access == 0 ||
				(ready && (access & DCAMPROP_ATTR_ACCESSREADY)) ||
				(!ready && (access & DCAMPROP_ATTR_ACCESSBUSY));
		*info = (struct hamamatsu_feature_info) {
		.name = feature->name,
		.property_name = feature->property_name,
		.description = feature->description,
		.kind = feature->kind,
		.n_enum_entries = feature->n_entries,
		.readable = (attribute.attribute & DCAMPROP_ATTR_READABLE) != 0 && state_access,
		.writable = (attribute.attribute & DCAMPROP_ATTR_WRITABLE) != 0 && state_access,
		.changes_layout = feature->changes_layout,
		};
	}
	info->available = info->readable || info->writable;
	return 0;
}

const char *hamamatsu_camera_get_feature_enum_entry(
		const struct hamamatsu_camera *camera, uint32_t index,
		uint32_t entry_index)
{
	if (camera == NULL || index >= camera->n_features ||
			entry_index >= camera->features[index].n_entries)
		return NULL;
	return camera->features[index].entries[entry_index].label;
}

int hamamatsu_camera_get_feature_value(struct hamamatsu_camera *camera,
		uint32_t index, struct hamamatsu_feature_value *value)
{
	struct dcam_feature *feature;
	double raw;
	uint32_t i;
	int result;

	if (camera == NULL || value == NULL || index >= camera->n_features)
		return -EINVAL;
	feature = &camera->features[index];
	if ((result = get_property(camera, feature->id, &raw)) < 0)
		return result;
	value->kind = feature->kind;
	switch (feature->kind) {
	case HAMAMATSU_FEATURE_INTEGER: value->integer = (int64_t)llround(raw); break;
	case HAMAMATSU_FEATURE_FLOATING: value->floating = raw; break;
	case HAMAMATSU_FEATURE_ENUMERATION:
		for (i = 0; i < feature->n_entries; i++) {
			if (feature->entries[i].value == raw) {
				value->enumeration = (int32_t)i;
				return 0;
			}
		}
		return -ENODATA;
	default: return -ENOTSUP;
	}
	return 0;
}

static int feature_range(struct hamamatsu_camera *camera, uint32_t index,
		DCAMPROP_ATTR *attribute)
{
	if (camera == NULL || attribute == NULL || index >= camera->n_features)
		return -EINVAL;
	return current_attribute(camera, &camera->features[index], attribute);
}

int hamamatsu_camera_get_feature_integer_range(struct hamamatsu_camera *camera,
		uint32_t index, int64_t *minimum, int64_t *maximum)
{
	DCAMPROP_ATTR attribute;
	int result;

	if (minimum == NULL || maximum == NULL ||
			(result = feature_range(camera, index, &attribute)) < 0)
		return minimum == NULL || maximum == NULL ? -EINVAL : result;
	*minimum = (int64_t)llround(attribute.valuemin);
	*maximum = (int64_t)llround(attribute.valuemax);
	return 0;
}

int hamamatsu_camera_get_feature_float_range(struct hamamatsu_camera *camera,
		uint32_t index, double *minimum, double *maximum)
{
	DCAMPROP_ATTR attribute;
	int result;

	if (minimum == NULL || maximum == NULL ||
			(result = feature_range(camera, index, &attribute)) < 0)
		return minimum == NULL || maximum == NULL ? -EINVAL : result;
	*minimum = attribute.valuemin;
	*maximum = attribute.valuemax;
	return 0;
}

int hamamatsu_camera_find_feature(const struct hamamatsu_camera *camera,
		const char *property_name, uint32_t *index)
{
	uint32_t i;

	if (camera == NULL || property_name == NULL || index == NULL)
		return -EINVAL;
	for (i = 0; i < camera->n_features; i++) {
		if (strcmp(camera->features[i].property_name, property_name) == 0) {
			*index = i;
			return 0;
		}
	}
	if (strcmp(property_name, "hamamatsu-command.SoftwareTrigger") == 0) {
		*index = camera->n_features;
		return 0;
	}
	return -ENOENT;
}

int hamamatsu_camera_set_feature_value(struct hamamatsu_camera *camera,
		uint32_t index, const struct hamamatsu_feature_value *value)
{
	struct dcam_feature *feature;
	double raw;
	DCAMERR error;

	if (camera == NULL || value == NULL || index > camera->n_features)
		return -EINVAL;
	if (index == camera->n_features) {
		if (value->kind != HAMAMATSU_FEATURE_COMMAND || !value->boolean)
			return -EINVAL;
		error = dcamcap_firetrigger(camera->handle, 0);
		return dcam_succeeded(error) ? 0 : dcam_error(error);
	}
	feature = &camera->features[index];
	if (value->kind != feature->kind)
		return -EINVAL;
	switch (feature->kind) {
	case HAMAMATSU_FEATURE_INTEGER: raw = (double)value->integer; break;
	case HAMAMATSU_FEATURE_FLOATING: raw = value->floating; break;
	case HAMAMATSU_FEATURE_ENUMERATION:
		if (value->enumeration < 0 ||
				(uint32_t)value->enumeration >= feature->n_entries)
			return -ERANGE;
		raw = feature->entries[value->enumeration].value;
		break;
	default: return -ENOTSUP;
	}
	error = dcamprop_setgetvalue(camera->handle, feature->id, &raw, 0);
	return dcam_succeeded(error) ? 0 : dcam_error(error);
}

int hamamatsu_camera_refresh_info(struct hamamatsu_camera *camera)
{
	if (camera == NULL)
		return -EINVAL;
	if (camera->started || camera->n_buffers != 0)
		return -EBUSY;
	return refresh_info(camera);
}

int hamamatsu_camera_announce(struct hamamatsu_camera *camera, void *memory,
		uint64_t size, void *user_data,
		struct hamamatsu_camera_buffer **buffer_ptr)
{
	struct hamamatsu_camera_buffer *buffer;

	if (camera == NULL || memory == NULL || buffer_ptr == NULL ||
			size < camera->info.payload_size ||
			camera->n_buffers >= HAMAMATSU_CAMERA_MAX_BUFFERS)
		return -EINVAL;
	buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL)
		return -errno;
	buffer->memory = memory;
	buffer->size = size;
	buffer->user_data = user_data;
	camera->buffers[camera->n_buffers++] = buffer;
	*buffer_ptr = buffer;
	return 0;
}

int hamamatsu_camera_revoke(struct hamamatsu_camera *camera,
		struct hamamatsu_camera_buffer **buffer_ptr)
{
	uint32_t i;

	if (camera == NULL || buffer_ptr == NULL || *buffer_ptr == NULL ||
			camera->started)
		return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++) {
		if (camera->buffers[i] != *buffer_ptr)
			continue;
		free(*buffer_ptr);
		*buffer_ptr = NULL;
		memmove(&camera->buffers[i], &camera->buffers[i + 1],
				(camera->n_buffers - i - 1u) * sizeof(camera->buffers[0]));
		camera->n_buffers--;
		return 0;
	}
	return -ENOENT;
}

int hamamatsu_camera_queue(struct hamamatsu_camera *camera,
		struct hamamatsu_camera_buffer *buffer)
{
	uint32_t i;

	if (camera == NULL || buffer == NULL || buffer->queued)
		return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++) {
		if (camera->buffers[i] == buffer) {
			buffer->queued = true;
			return 0;
		}
	}
	return -ENOENT;
}

static void release_capture_buffers(struct hamamatsu_camera *camera)
{
	if (camera->handle != NULL && camera->capture_buffers_allocated)
		(void)dcambuf_release(camera->handle, DCAMBUF_ATTACHKIND_FRAME);
	camera->capture_buffers_allocated = false;
}

int hamamatsu_camera_start(struct hamamatsu_camera *camera)
{
	DCAMERR error;

	if (camera == NULL || camera->started || camera->n_buffers < 2)
		return -EINVAL;
	error = dcambuf_alloc(camera->handle, (int32)DCAM_RING_FRAMES);
	if (!dcam_succeeded(error))
		return dcam_error(error);
	camera->capture_buffers_allocated = true;
	error = dcamcap_start(camera->handle, DCAMCAP_START_SEQUENCE);
	if (!dcam_succeeded(error)) {
		release_capture_buffers(camera);
		return dcam_error(error);
	}
	camera->last_frame_count = 0;
	camera->started = true;
	return 0;
}

int hamamatsu_camera_stop(struct hamamatsu_camera *camera)
{
	uint32_t i;

	if (camera == NULL)
		return -EINVAL;
	if (camera->started) {
		DCAMERR error = dcamcap_stop(camera->handle);
		if (!dcam_succeeded(error))
			return dcam_error(error);
	}
	camera->started = false;
	release_capture_buffers(camera);
	for (i = 0; i < camera->n_buffers; i++)
		camera->buffers[i]->queued = false;
	return 0;
}

int hamamatsu_camera_try_get_completion(struct hamamatsu_camera *camera,
		struct hamamatsu_camera_completion *completion)
{
	DCAMCAP_TRANSFERINFO transfer = {
		.size = sizeof(transfer), .iKind = DCAMCAP_TRANSFERKIND_FRAME,
	};
	struct hamamatsu_camera_buffer *output = NULL;
	DCAMBUF_FRAME frame;
	uint32_t i;
	DCAMERR error;

	if (camera == NULL || completion == NULL || !camera->started)
		return -EINVAL;
	error = dcamcap_transferinfo(camera->handle, &transfer);
	if (!dcam_succeeded(error))
		return dcam_error(error);
	if (transfer.nFrameCount <= camera->last_frame_count ||
			transfer.nNewestFrameIndex < 0)
		return 0;
	for (i = 0; i < camera->n_buffers; i++) {
		uint32_t index = (camera->scan_hint + i) % camera->n_buffers;
		if (camera->buffers[index]->queued) {
			output = camera->buffers[index];
			camera->scan_hint = (index + 1u) % camera->n_buffers;
			break;
		}
	}
	if (output == NULL)
		return 0;
	memset(&frame, 0, sizeof(frame));
	frame.size = sizeof(frame);
	frame.iFrame = transfer.nNewestFrameIndex;
	frame.buf = output->memory;
	frame.rowbytes = (int32)camera->info.stride;
	frame.width = (int32)camera->info.width;
	frame.height = (int32)camera->info.height;
	error = dcambuf_copyframe(camera->handle, &frame);
	if (!dcam_succeeded(error))
		return dcam_error(error);
	camera->last_frame_count = transfer.nFrameCount;
	output->queued = false;
	*completion = (struct hamamatsu_camera_completion) {
		.buffer = output,
		.user_data = output->user_data,
		.frame_id = (uint32_t)frame.framestamp == DCAMCONST_FRAMESTAMP_MISMATCH ?
				(uint32_t)transfer.nFrameCount : (uint32_t)frame.framestamp,
		.size_filled = camera->info.image_size,
		.incomplete = false,
	};
	return 1;
}
