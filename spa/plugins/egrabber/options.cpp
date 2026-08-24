/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "options.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>

#include <spa/utils/string.h>

#include "egrabber.hpp"

namespace egrabber_pipewire {
namespace {

template<typename T>
T parse_unsigned(const char *text, std::string_view key)
{
	std::uint64_t value = 0;
	const std::string_view input(text == nullptr ? "" : text);
	const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
	if (input.empty() || result.ec != std::errc{} ||
			result.ptr != input.data() + input.size() ||
			value > std::numeric_limits<T>::max())
		throw std::invalid_argument(std::string(key) +
				" requires an unsigned integer in range");
	return static_cast<T>(value);
}

std::array<std::uint8_t, 16> parse_acquisition_domain(const char *value)
{
	std::string text(value == nullptr ? "" : value);
	text.erase(std::remove(text.begin(), text.end(), '-'), text.end());
	if (text.size() != 32)
		throw std::invalid_argument("acquisition domain must contain 32 hexadecimal digits");
	std::array<std::uint8_t, 16> domain{};
	bool nonzero = false;
	for (std::size_t i = 0; i < domain.size(); ++i) {
		unsigned int byte = 0;
		const char *first = text.data() + i * 2;
		const auto result = std::from_chars(first, first + 2, byte, 16);
		if (result.ec != std::errc{} || result.ptr != first + 2)
			throw std::invalid_argument("acquisition domain contains a non-hexadecimal digit");
		domain[i] = static_cast<std::uint8_t>(byte);
		nonzero = nonzero || byte != 0;
	}
	if (!nonzero)
		throw std::invalid_argument("acquisition domain must not be all zero");
	return domain;
}

ProgressivePolicy parse_progressive_policy(const char *value)
{
	if (spa_streq(value, "disabled"))
		return ProgressivePolicy::disabled;
	if (spa_streq(value, "offer"))
		return ProgressivePolicy::offer;
	if (spa_streq(value, "require"))
		return ProgressivePolicy::require;
	throw std::invalid_argument("progressive policy must be disabled, offer, or require");
}

} // namespace

const char *progressive_policy_name(ProgressivePolicy policy) noexcept
{
	switch (policy) {
	case ProgressivePolicy::disabled:
		return "disabled";
	case ProgressivePolicy::offer:
		return "offer";
	case ProgressivePolicy::require:
		return "require";
	}
	return "disabled";
}

std::string format_acquisition_domain(
		const std::array<std::uint8_t, 16> &domain)
{
	static constexpr char hex[] = "0123456789abcdef";
	std::string result(domain.size() * 2, '0');
	for (std::size_t i = 0; i < domain.size(); ++i) {
		result[i * 2] = hex[domain[i] >> 4];
		result[i * 2 + 1] = hex[domain[i] & 0x0f];
	}
	return result;
}

void read_options(Options &options, const struct spa_dict *info)
{
	const char *value;

	if (info == nullptr)
		return;
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_PRODUCER)))
		options.producer = value;
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_SERIAL)))
		options.serial = value;
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_USER_ID)))
		options.user_id = value;
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_CONTROL)))
		options.control = value;
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_PROGRESSIVE)))
		options.progressive = parse_progressive_policy(value);
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_ACQUISITION_DOMAIN)))
		options.acquisition_domain = parse_acquisition_domain(value);
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_ACQUISITION_GENERATION)))
		options.acquisition_generation = parse_unsigned<std::uint64_t>(value,
				SPA_KEY_API_EGRABBER_ACQUISITION_GENERATION);
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_ACQUISITION_SEQUENCE_CONTEXT)))
		options.acquisition_sequence_context = parse_unsigned<std::uint32_t>(value,
				SPA_KEY_API_EGRABBER_ACQUISITION_SEQUENCE_CONTEXT);
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_INTERFACE_INDEX)))
		options.interface_index = parse_unsigned<int>(value,
				SPA_KEY_API_EGRABBER_INTERFACE_INDEX);
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_DEVICE_INDEX)))
		options.device_index = parse_unsigned<int>(value,
				SPA_KEY_API_EGRABBER_DEVICE_INDEX);
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_STREAM_INDEX)))
		options.stream_index = parse_unsigned<int>(value,
				SPA_KEY_API_EGRABBER_STREAM_INDEX);
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_BUFFER_COUNT)))
		options.buffer_count = parse_unsigned<std::size_t>(value,
				SPA_KEY_API_EGRABBER_BUFFER_COUNT);

	if (options.buffer_count < 2)
		throw std::invalid_argument("eGrabber buffer count must be at least two");
	if (options.control != "auto" && options.control != "remote" &&
			options.control != "clprotocol" && options.control != "none")
		throw std::invalid_argument("eGrabber control must be auto, remote, clprotocol, or none");
	if (options.acquisition_sequence_context > 3)
		throw std::invalid_argument("acquisition sequence context must be 1, 2, or 3");
	if (options.acquisition_domain.has_value() !=
			(options.acquisition_sequence_context != 0))
		throw std::invalid_argument("acquisition domain and sequence context must be used together");
	if (!options.acquisition_domain && options.acquisition_generation != 0)
		throw std::invalid_argument("acquisition generation requires an acquisition domain");
}

} // namespace egrabber_pipewire
