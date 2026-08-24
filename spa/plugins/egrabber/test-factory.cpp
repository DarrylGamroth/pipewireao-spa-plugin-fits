/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <dlfcn.h>

#include <spa/support/plugin.h>

#include "egrabber.hpp"

int main(int argc, char **argv)
{
	spa_assert_se(argc == 2);
	void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != nullptr);
	auto enumerate = reinterpret_cast<spa_handle_factory_enum_func_t>(
			dlsym(library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME));
	spa_assert_se(enumerate != nullptr);

	const char *expected[] = {
		SPA_NAME_API_EGRABBER_ENUM_MANAGER,
		SPA_NAME_API_EGRABBER_DEVICE,
		SPA_NAME_API_EGRABBER_SOURCE,
	};
	const struct spa_handle_factory *factory = nullptr;
	uint32_t index = 0;
	for (const char *name : expected) {
		spa_assert_se(enumerate(&factory, &index) == 1);
		spa_assert_se(factory != nullptr);
		spa_assert_se(spa_streq(factory->name, name));
	}
	spa_assert_se(index == SPA_N_ELEMENTS(expected));
	spa_assert_se(enumerate(&factory, &index) == 0);
	spa_assert_se(dlclose(library) == 0);
	return 0;
}
