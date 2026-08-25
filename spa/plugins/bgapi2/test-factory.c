/* SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>

#include <spa/support/plugin.h>

#include "bgapi2.h"

int main(int argc, char *argv[])
{
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	const struct spa_handle_factory *source_factory = NULL;
	const char *expected[] = {
		SPA_NAME_API_BGAPI2_ENUM_MANAGER,
		SPA_NAME_API_BGAPI2_DEVICE,
		SPA_NAME_API_BGAPI2_SOURCE,
	};
	uint32_t index = 0;
	void *library;

	spa_assert_se(argc == 2);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	enumerate = (spa_handle_factory_enum_func_t)dlsym(library,
			SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(enumerate != NULL);
	for (uint32_t i = 0; i < SPA_N_ELEMENTS(expected); i++) {
		spa_assert_se(enumerate(&factory, &index) == 1);
		spa_assert_se(factory != NULL);
		spa_assert_se(spa_streq(factory->name, expected[i]));
		if (spa_streq(factory->name, SPA_NAME_API_BGAPI2_SOURCE))
			source_factory = factory;
	}
	spa_assert_se(source_factory != NULL);
	spa_assert_se(enumerate(&factory, &index) == 0);
	{
		struct spa_handle *handle = calloc(1,
				source_factory->get_size(source_factory, NULL));
		spa_assert_se(handle != NULL);
		spa_assert_se(source_factory->init(source_factory, handle,
				NULL, NULL, 0) == -EINVAL);
		free(handle);
	}
	{
		const struct spa_dict_item items[] = {
			SPA_DICT_ITEM_INIT(SPA_KEY_API_BGAPI2_PRODUCER, "/invalid.cti"),
			SPA_DICT_ITEM_INIT(SPA_KEY_API_BGAPI2_INTERFACE_INDEX, "invalid"),
		};
		const struct spa_dict info = SPA_DICT_INIT(items,
				SPA_N_ELEMENTS(items));
		struct spa_handle *handle = calloc(1,
				source_factory->get_size(source_factory, &info));
		spa_assert_se(handle != NULL);
		spa_assert_se(source_factory->init(source_factory, handle,
				&info, NULL, 0) == -EINVAL);
		free(handle);
	}
	{
		const struct spa_dict_item items[] = {
			SPA_DICT_ITEM_INIT(SPA_KEY_API_BGAPI2_PRODUCER, "/invalid.cti"),
			SPA_DICT_ITEM_INIT(SPA_KEY_API_BGAPI2_READINESS, "invalid"),
		};
		const struct spa_dict info = SPA_DICT_INIT(items,
				SPA_N_ELEMENTS(items));
		struct spa_handle *handle = calloc(1,
				source_factory->get_size(source_factory, &info));
		spa_assert_se(handle != NULL);
		spa_assert_se(source_factory->init(source_factory, handle,
				&info, NULL, 0) == -EINVAL);
		free(handle);
	}
	{
		const struct spa_dict_item items[] = {
			SPA_DICT_ITEM_INIT(SPA_KEY_API_BGAPI2_PRODUCER, "/invalid.cti"),
			SPA_DICT_ITEM_INIT(SPA_KEY_API_BGAPI2_READINESS, "eventfd"),
		};
		const struct spa_dict info = SPA_DICT_INIT(items,
				SPA_N_ELEMENTS(items));
		struct spa_handle *handle = calloc(1,
				source_factory->get_size(source_factory, &info));
		spa_assert_se(handle != NULL);
		spa_assert_se(source_factory->init(source_factory, handle,
				&info, NULL, 0) == -ENOTSUP);
		free(handle);
	}
	spa_assert_se(dlclose(library) == 0);
	return 0;
}
