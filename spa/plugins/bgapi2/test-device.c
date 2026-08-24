/* SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <spa/monitor/device.h>
#include <spa/node/node.h>
#include <spa/support/plugin.h>
#include <spa/utils/keys.h>

#include "bgapi2.h"

struct observation {
	uint32_t objects;
	uint32_t results;
	int sequence;
	int result;
	char type[128];
	char factory[128];
	char producer[PATH_MAX];
	char serial[128];
	char interface_index[16];
	char device_index[16];
	char stream_index[16];
	char transport[64];
	char path[PATH_MAX + 384];
	char name[256];
	char description[320];
	char vendor[128];
	char model[128];
};

static void copy_text(char *destination, size_t capacity, const char *value)
{
	if (value == NULL)
		value = "";
	spa_assert_se(strlen(value) < capacity);
	memcpy(destination, value, strlen(value) + 1);
}

static void copy_value(char *destination, size_t capacity,
		const struct spa_dict *props, const char *key)
{
	copy_text(destination, capacity,
			props == NULL ? NULL : spa_dict_lookup(props, key));
}

static void on_object_info(void *data, uint32_t id SPA_UNUSED,
		const struct spa_device_object_info *info)
{
	struct observation *observation = data;

	if (info == NULL)
		return;
	observation->objects++;
	if (observation->objects != 1)
		return;
	copy_text(observation->type, sizeof(observation->type), info->type);
	copy_text(observation->factory, sizeof(observation->factory),
			info->factory_name);
	copy_value(observation->producer, sizeof(observation->producer), info->props,
			SPA_KEY_API_BGAPI2_PRODUCER);
	copy_value(observation->serial, sizeof(observation->serial), info->props,
			SPA_KEY_API_BGAPI2_SERIAL);
	copy_value(observation->interface_index,
			sizeof(observation->interface_index), info->props,
			SPA_KEY_API_BGAPI2_INTERFACE_INDEX);
	copy_value(observation->device_index, sizeof(observation->device_index),
			info->props, SPA_KEY_API_BGAPI2_DEVICE_INDEX);
	copy_value(observation->stream_index, sizeof(observation->stream_index),
			info->props, SPA_KEY_API_BGAPI2_STREAM_INDEX);
	copy_value(observation->transport, sizeof(observation->transport),
			info->props, SPA_KEY_API_BGAPI2_TRANSPORT);
	copy_value(observation->path, sizeof(observation->path), info->props,
			SPA_KEY_OBJECT_PATH);
	copy_value(observation->name, sizeof(observation->name), info->props,
			SPA_KEY_DEVICE_NAME);
	copy_value(observation->description, sizeof(observation->description),
			info->props, SPA_KEY_DEVICE_DESCRIPTION);
	copy_value(observation->vendor, sizeof(observation->vendor), info->props,
			SPA_KEY_DEVICE_VENDOR_NAME);
	copy_value(observation->model, sizeof(observation->model), info->props,
			SPA_KEY_DEVICE_PRODUCT_NAME);
}

static void on_result(void *data, int seq, int result,
		uint32_t type SPA_UNUSED, const void *value SPA_UNUSED)
{
	struct observation *observation = data;

	observation->results++;
	observation->sequence = seq;
	observation->result = result;
}

static const struct spa_device_events device_events = {
	.version = SPA_VERSION_DEVICE_EVENTS,
	.result = on_result,
	.object_info = on_object_info,
};

static const struct spa_handle_factory *find_factory(
		spa_handle_factory_enum_func_t enumerate, const char *name)
{
	const struct spa_handle_factory *factory = NULL;
	uint32_t index = 0;

	while (enumerate(&factory, &index) > 0)
		if (spa_streq(factory->name, name))
			return factory;
	return NULL;
}

static struct spa_handle *create_instance(
		const struct spa_handle_factory *factory, const struct spa_dict *info,
		struct spa_device **device)
{
	struct spa_handle *handle = calloc(1, factory->get_size(factory, info));

	spa_assert_se(handle != NULL);
	spa_assert_se(factory->init(factory, handle, info, NULL, 0) == 0);
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Device,
			(void **)device) == 0);
	return handle;
}

int main(int argc, char *argv[])
{
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *manager_factory, *device_factory;
	struct observation manager_observation = { 0 };
	struct observation device_observation = { 0 };
	struct spa_device *manager, *device;
	struct spa_handle *manager_handle, *device_handle;
	struct spa_hook manager_listener, device_listener;
	void *library;

	spa_assert_se(argc == 3);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	enumerate = (spa_handle_factory_enum_func_t)dlsym(library,
			SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(enumerate != NULL);
	manager_factory = find_factory(enumerate, SPA_NAME_API_BGAPI2_ENUM_MANAGER);
	device_factory = find_factory(enumerate, SPA_NAME_API_BGAPI2_DEVICE);
	spa_assert_se(manager_factory != NULL && device_factory != NULL);

	const struct spa_dict_item manager_items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_BGAPI2_PRODUCER, argv[2]),
	};
	const struct spa_dict manager_info = SPA_DICT_INIT_ARRAY(manager_items);
	manager_handle = create_instance(manager_factory, &manager_info, &manager);
	spa_assert_se(spa_device_add_listener(manager, &manager_listener,
			&device_events, &manager_observation) == 0);
	if (manager_observation.objects == 0) {
		spa_hook_remove(&manager_listener);
		spa_assert_se(manager_handle->clear(manager_handle) == 0);
		free(manager_handle);
		spa_assert_se(dlclose(library) == 0);
		return 77;
	}
	spa_assert_se(spa_streq(manager_observation.type, SPA_TYPE_INTERFACE_Device));
	spa_assert_se(spa_streq(manager_observation.factory,
			SPA_NAME_API_BGAPI2_DEVICE));
	spa_assert_se(manager_observation.producer[0] != '\0');
	spa_assert_se(manager_observation.serial[0] != '\0');
	spa_assert_se(manager_observation.interface_index[0] != '\0');
	spa_assert_se(manager_observation.device_index[0] != '\0');
	spa_assert_se(manager_observation.stream_index[0] != '\0');
	spa_assert_se(spa_device_sync(manager, 9) == 0);
	spa_assert_se(manager_observation.results == 1);
	spa_assert_se(manager_observation.sequence == 9);
	spa_assert_se(manager_observation.result == 0);
	spa_assert_se(manager_observation.objects == 1);

	const struct spa_dict_item device_items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_BGAPI2_PRODUCER,
				manager_observation.producer),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_BGAPI2_SERIAL,
				manager_observation.serial),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_BGAPI2_INTERFACE_INDEX,
				manager_observation.interface_index),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_BGAPI2_DEVICE_INDEX,
				manager_observation.device_index),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_BGAPI2_STREAM_INDEX,
				manager_observation.stream_index),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_BGAPI2_TRANSPORT,
				manager_observation.transport),
		SPA_DICT_ITEM_INIT(SPA_KEY_OBJECT_PATH, manager_observation.path),
		SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_NAME, manager_observation.name),
		SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_DESCRIPTION,
				manager_observation.description),
		SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_VENDOR_NAME,
				manager_observation.vendor),
		SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_PRODUCT_NAME,
				manager_observation.model),
	};
	const struct spa_dict device_info = SPA_DICT_INIT_ARRAY(device_items);
	device_handle = create_instance(device_factory, &device_info, &device);
	spa_assert_se(spa_device_add_listener(device, &device_listener,
			&device_events, &device_observation) == 0);
	spa_assert_se(device_observation.objects == 1);
	spa_assert_se(spa_streq(device_observation.type, SPA_TYPE_INTERFACE_Node));
	spa_assert_se(spa_streq(device_observation.factory,
			SPA_NAME_API_BGAPI2_SOURCE));
	spa_assert_se(spa_streq(device_observation.producer,
			manager_observation.producer));
	spa_assert_se(spa_streq(device_observation.serial,
			manager_observation.serial));

	spa_hook_remove(&device_listener);
	spa_assert_se(device_handle->clear(device_handle) == 0);
	free(device_handle);
	spa_hook_remove(&manager_listener);
	spa_assert_se(manager_handle->clear(manager_handle) == 0);
	free(manager_handle);
	spa_assert_se(dlclose(library) == 0);
	return 0;
}
