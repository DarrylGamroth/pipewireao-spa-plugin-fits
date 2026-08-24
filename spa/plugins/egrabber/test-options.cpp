/* SPDX-License-Identifier: MIT */

#include "egrabber.hpp"
#include "options.hpp"

#include <cassert>
#include <stdexcept>

using egrabber_pipewire::Options;

Options parse(std::initializer_list<struct spa_dict_item> items)
{
	const struct spa_dict dict = SPA_DICT_INIT(items.begin(),
			static_cast<uint32_t>(items.size()));
	Options options;
	egrabber_pipewire::read_options(options, &dict);
	return options;
}

template<typename Function>
bool throws(Function &&function)
{
	try {
		function();
	} catch (const std::invalid_argument &) {
		return true;
	}
	return false;
}

int main()
{
	const auto timed = parse({
		{ SPA_KEY_API_EGRABBER_PRODUCER, "coaxlink" },
		{ SPA_KEY_API_EGRABBER_PROGRESSIVE, "require" },
		{ SPA_KEY_API_EGRABBER_ACQUISITION_DOMAIN,
				"00112233-4455-6677-8899-aabbccddeeff" },
		{ SPA_KEY_API_EGRABBER_ACQUISITION_GENERATION, "42" },
		{ SPA_KEY_API_EGRABBER_ACQUISITION_SEQUENCE_CONTEXT, "2" },
		{ SPA_KEY_API_EGRABBER_BUFFER_COUNT, "12" },
	});
	assert(timed.producer == "coaxlink");
	assert(timed.progressive == egrabber_pipewire::ProgressivePolicy::require);
	assert(timed.acquisition_domain);
	assert((*timed.acquisition_domain)[0] == 0x00);
	assert((*timed.acquisition_domain)[15] == 0xff);
	assert(egrabber_pipewire::format_acquisition_domain(
			*timed.acquisition_domain) == "00112233445566778899aabbccddeeff");
	assert(timed.acquisition_generation == 42);
	assert(timed.acquisition_sequence_context == 2);
	assert(timed.buffer_count == 12);

	assert(throws([] { parse({{ SPA_KEY_API_EGRABBER_BUFFER_COUNT, "1" }}); }));
	assert(throws([] { parse({{ SPA_KEY_API_EGRABBER_BUFFER_COUNT, "8frames" }}); }));
	assert(throws([] { parse({{ SPA_KEY_API_EGRABBER_PROGRESSIVE, "sometimes" }}); }));
	assert(throws([] { parse({{
		SPA_KEY_API_EGRABBER_ACQUISITION_DOMAIN, "0011" }}); }));
	assert(throws([] { parse({{
		SPA_KEY_API_EGRABBER_ACQUISITION_DOMAIN,
		"00000000000000000000000000000000" }}); }));
	assert(throws([] { parse({{
		SPA_KEY_API_EGRABBER_ACQUISITION_DOMAIN,
		"00112233445566778899aabbccddeeff" }}); }));
	assert(throws([] { parse({{
		SPA_KEY_API_EGRABBER_ACQUISITION_SEQUENCE_CONTEXT, "4" }}); }));
	assert(throws([] { parse({{
		SPA_KEY_API_EGRABBER_ACQUISITION_GENERATION, "42frames" }}); }));
}
