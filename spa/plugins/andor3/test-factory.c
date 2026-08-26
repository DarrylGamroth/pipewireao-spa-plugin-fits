/* SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>

#include <spa/support/plugin.h>

#include "andor3.h"

static int initialize(const struct spa_handle_factory *factory,
		const struct spa_dict *info)
{
	struct spa_handle *handle = calloc(1, factory->get_size(factory, info));
	int res;

	spa_assert_se(handle != NULL);
	res = factory->init(factory, handle, info, NULL, 0);
	if (res == 0)
		spa_assert_se(spa_handle_clear(handle) == 0);
	free(handle);
	return res;
}

int main(int argc, char *argv[])
{
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	bool real_sdk;
	uint32_t index = 0;
	void *library;

	spa_assert_se(argc == 2 || argc == 3);
	real_sdk = argc == 3;
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	enumerate = (spa_handle_factory_enum_func_t)dlsym(library,
			SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(enumerate != NULL);
	spa_assert_se(enumerate(&factory, &index) == 1);
	spa_assert_se(factory != NULL &&
			spa_streq(factory->name, SPA_NAME_API_ANDOR3_SOURCE));
	spa_assert_se(enumerate(&factory, &index) == 0);
	spa_assert_se(initialize(factory, NULL) == 0);
	{
		const struct spa_dict_item items[] = {
			SPA_DICT_ITEM_INIT(SPA_KEY_API_ANDOR3_DEVICE_INDEX, "0"),
		};
		const struct spa_dict info = SPA_DICT_INIT(items,
				SPA_N_ELEMENTS(items));

		spa_assert_se(initialize(factory, &info) == 0);
	}
	if (!real_sdk) {
		const struct spa_dict_item items[] = {
			SPA_DICT_ITEM_INIT(SPA_KEY_API_ANDOR3_DEVICE_INDEX, "1"),
		};
		const struct spa_dict info = SPA_DICT_INIT(items,
				SPA_N_ELEMENTS(items));

		spa_assert_se(initialize(factory, &info) == -ENODEV);
	}
	{
		const struct spa_dict_item items[] = {
			SPA_DICT_ITEM_INIT(SPA_KEY_API_ANDOR3_DEVICE_INDEX, "invalid"),
		};
		const struct spa_dict info = SPA_DICT_INIT(items,
				SPA_N_ELEMENTS(items));

		spa_assert_se(initialize(factory, &info) == -EINVAL);
	}
	{
		const struct spa_dict_item items[] = {
			SPA_DICT_ITEM_INIT(SPA_KEY_API_ANDOR3_READINESS, "eventfd"),
		};
		const struct spa_dict info = SPA_DICT_INIT(items,
				SPA_N_ELEMENTS(items));

		spa_assert_se(initialize(factory, &info) == -EINVAL);
	}
	spa_assert_se(dlclose(library) == 0);
	return 0;
}
