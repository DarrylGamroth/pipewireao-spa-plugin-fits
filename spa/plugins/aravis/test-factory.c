/* SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>

#include <spa/support/plugin.h>

#include "aravis.h"

int main(int argc, char *argv[])
{
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	uint32_t index = 0;
	void *library;

	spa_assert_se(argc == 2);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	enumerate = (spa_handle_factory_enum_func_t)dlsym(library,
			SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(enumerate != NULL);
	spa_assert_se(enumerate(&factory, &index) == 1);
	spa_assert_se(factory != NULL);
	spa_assert_se(spa_streq(factory->name, SPA_NAME_API_ARAVIS_SOURCE));
	spa_assert_se(index == 1);
	spa_assert_se(enumerate(&factory, &index) == 0);
	{
		struct spa_handle *handle = calloc(1,
				factory->get_size(factory, NULL));
		spa_assert_se(handle != NULL);
		spa_assert_se(factory->init(factory, handle, NULL, NULL, 0) == -EINVAL);
		free(handle);
	}
	spa_assert_se(dlclose(library) == 0);
	return 0;
}
