/* SPDX-License-Identifier: MIT */
#include "bgapi2.h"
#include "camera.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>

struct camera_descriptor {
	struct bgapi2_discovered_device camera;
	char interface_index[16];
	char device_index[16];
	char stream_index[16];
	char name[256];
	char description[320];
	char path[PATH_MAX + 384];
};

struct impl {
	struct spa_handle handle;
	struct spa_device device;
	struct spa_hook_list hooks;
	struct spa_device_info info;
	char producer[PATH_MAX];
	struct camera_descriptor *cameras;
	uint32_t n_cameras;
	uint32_t capacity;
};

static int copy_string(char *destination, size_t capacity, const char *source)
{
	int written;

	if (source == NULL)
		source = "";
	written = snprintf(destination, capacity, "%s", source);
	return written < 0 || (size_t)written >= capacity ? -ENAMETOOLONG : 0;
}

static int add_camera(void *data,
		const struct bgapi2_discovered_device *camera)
{
	struct impl *this = data;
	struct camera_descriptor *descriptor;
	char stable[384];
	uint32_t capacity;
	void *storage;
	int written;

	if (this->n_cameras == this->capacity) {
		capacity = this->capacity == 0 ? 8u : this->capacity * 2u;
		storage = realloc(this->cameras,
				capacity * sizeof(struct camera_descriptor));
		if (storage == NULL)
			return -ENOMEM;
		this->cameras = storage;
		this->capacity = capacity;
	}
	descriptor = &this->cameras[this->n_cameras];
	memset(descriptor, 0, sizeof(*descriptor));
	descriptor->camera = *camera;
	snprintf(descriptor->interface_index, sizeof(descriptor->interface_index),
			"%u", camera->interface_index);
	snprintf(descriptor->device_index, sizeof(descriptor->device_index),
			"%u", camera->device_index);
	snprintf(descriptor->stream_index, sizeof(descriptor->stream_index), "0");
	if (camera->serial[0] != '\0')
		written = snprintf(stable, sizeof(stable), "%s", camera->serial);
	else
		written = snprintf(stable, sizeof(stable), "%u.%u.0",
				camera->interface_index, camera->device_index);
	if (written < 0 || (size_t)written >= sizeof(stable))
		return -ENAMETOOLONG;
	written = snprintf(descriptor->name, sizeof(descriptor->name),
			"bgapi2_device.%s", stable);
	if (written < 0 || (size_t)written >= sizeof(descriptor->name))
		return -ENAMETOOLONG;
	written = snprintf(descriptor->description, sizeof(descriptor->description),
			"%s%s%s", camera->vendor,
			camera->vendor[0] != '\0' && camera->model[0] != '\0' ? " " : "",
			camera->model);
	if (written < 0 || (size_t)written >= sizeof(descriptor->description))
		return -ENAMETOOLONG;
	if (descriptor->description[0] == '\0')
		copy_string(descriptor->description, sizeof(descriptor->description),
				"BGAPI2 camera");
	written = snprintf(descriptor->path, sizeof(descriptor->path),
			"bgapi2:%s:%s", this->producer, stable);
	if (written < 0 || (size_t)written >= sizeof(descriptor->path))
		return -ENAMETOOLONG;
	for (uint32_t i = 0; i < this->n_cameras; i++)
		if (spa_streq(this->cameras[i].path, descriptor->path))
			return -EEXIST;
	this->n_cameras++;
	return 0;
}

static void emit_camera(struct impl *this, uint32_t id)
{
	const struct camera_descriptor *camera = &this->cameras[id];
	struct spa_dict_item items[16];
	uint32_t n_items = 0;

#define ADD_ITEM(key, value) items[n_items++] = SPA_DICT_ITEM_INIT((key), (value))
	ADD_ITEM(SPA_KEY_DEVICE_ENUM_API, "bgapi2.manager");
	ADD_ITEM(SPA_KEY_DEVICE_API, "bgapi2");
	ADD_ITEM(SPA_KEY_MEDIA_CLASS, "Video/Device");
	ADD_ITEM(SPA_KEY_OBJECT_PATH, camera->path);
	ADD_ITEM(SPA_KEY_DEVICE_NAME, camera->name);
	ADD_ITEM(SPA_KEY_DEVICE_DESCRIPTION, camera->description);
	ADD_ITEM(SPA_KEY_API_BGAPI2_PRODUCER, this->producer);
	ADD_ITEM(SPA_KEY_API_BGAPI2_INTERFACE_INDEX, camera->interface_index);
	ADD_ITEM(SPA_KEY_API_BGAPI2_DEVICE_INDEX, camera->device_index);
	ADD_ITEM(SPA_KEY_API_BGAPI2_STREAM_INDEX, camera->stream_index);
	if (camera->camera.vendor[0] != '\0')
		ADD_ITEM(SPA_KEY_DEVICE_VENDOR_NAME, camera->camera.vendor);
	if (camera->camera.model[0] != '\0')
		ADD_ITEM(SPA_KEY_DEVICE_PRODUCT_NAME, camera->camera.model);
	if (camera->camera.serial[0] != '\0') {
		ADD_ITEM(SPA_KEY_DEVICE_SERIAL, camera->camera.serial);
		ADD_ITEM(SPA_KEY_API_BGAPI2_SERIAL, camera->camera.serial);
	}
	if (camera->camera.transport[0] != '\0')
		ADD_ITEM(SPA_KEY_API_BGAPI2_TRANSPORT, camera->camera.transport);
#undef ADD_ITEM

	struct spa_dict props = SPA_DICT_INIT(items, n_items);
	struct spa_device_object_info info = SPA_DEVICE_OBJECT_INFO_INIT();
	info.type = SPA_TYPE_INTERFACE_Device;
	info.factory_name = SPA_NAME_API_BGAPI2_DEVICE;
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_FLAGS |
			SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.props = &props;
	spa_device_emit_object_info(&this->hooks, id, &info);
}

static void emit_info(struct impl *this, bool full)
{
	static const struct spa_dict_item items[] = {
		{ SPA_KEY_DEVICE_API, "bgapi2" },
		{ SPA_KEY_DEVICE_NICK, "BGAPI2 manager" },
	};
	static const struct spa_dict props = SPA_DICT_INIT_ARRAY(items);
	uint64_t old = full ? this->info.change_mask : 0;

	if (full)
		this->info.change_mask = SPA_DEVICE_CHANGE_MASK_FLAGS |
				SPA_DEVICE_CHANGE_MASK_PROPS;
	if (this->info.change_mask != 0) {
		this->info.props = &props;
		spa_device_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static int impl_add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;
	uint32_t i;

	spa_return_val_if_fail(this != NULL && events != NULL, -EINVAL);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);
	emit_info(this, true);
	for (i = 0; i < this->n_cameras; i++)
		emit_camera(this, i);
	spa_hook_list_join(&this->hooks, &save);
	return 0;
}

static int impl_sync(void *object, int seq)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_device_emit_result(&this->hooks, seq, 0, 0, NULL);
	return 0;
}

static const struct spa_device_methods device_methods = {
	.version = SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_add_listener,
	.sync = impl_sync,
};

static int impl_get_interface(struct spa_handle *handle, const char *type,
		void **interface)
{
	struct impl *this = (struct impl *)handle;

	spa_return_val_if_fail(this != NULL && interface != NULL, -EINVAL);
	if (!spa_streq(type, SPA_TYPE_INTERFACE_Device))
		return -ENOENT;
	*interface = &this->device;
	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this = (struct impl *)handle;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	free(this->cameras);
	return 0;
}

static size_t impl_get_size(const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_dict *params SPA_UNUSED)
{
	return sizeof(struct impl);
}

static int impl_init(const struct spa_handle_factory *factory SPA_UNUSED,
		struct spa_handle *handle, const struct spa_dict *info,
		const struct spa_support *support SPA_UNUSED,
		uint32_t n_support SPA_UNUSED)
{
	struct impl *this = (struct impl *)handle;
	const char *producer;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	memset(this, 0, sizeof(*this));
	producer = info == NULL ? NULL :
			spa_dict_lookup(info, SPA_KEY_API_BGAPI2_PRODUCER);
	if (producer == NULL || copy_string(this->producer,
			sizeof(this->producer), producer) < 0)
		return -EINVAL;
	this->handle.get_interface = impl_get_interface;
	this->handle.clear = impl_clear;
	spa_hook_list_init(&this->hooks);
	this->info = SPA_DEVICE_INFO_INIT();
	this->device.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE, &device_methods, this);
	res = bgapi2_camera_discover(this->producer, 100, 200, add_camera, this);
	if (res < 0) {
		free(this->cameras);
		this->cameras = NULL;
		return res;
	}
	return 0;
}

static const struct spa_interface_info interfaces[] = {
	{ SPA_TYPE_INTERFACE_Device, },
};

static int impl_enum_interface_info(
		const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(info != NULL && index != NULL, -EINVAL);
	if (*index >= SPA_N_ELEMENTS(interfaces))
		return 0;
	*info = &interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory spa_bgapi2_manager_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BGAPI2_ENUM_MANAGER,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
