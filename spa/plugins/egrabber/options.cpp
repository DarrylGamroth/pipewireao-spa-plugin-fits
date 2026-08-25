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

OutputMode parse_output_mode(const char *value)
{
	if (spa_streq(value, "frame"))
		return OutputMode::frame;
	if (spa_streq(value, "row-block"))
		return OutputMode::row_block;
	throw std::invalid_argument("eGrabber output mode must be frame or row-block");
}

std::vector<std::string> parse_clprotocol_libraries(const char *value)
{
	const std::string_view input(value == nullptr ? "" : value);
	if (input.empty())
		throw std::invalid_argument("CLProtocol libraries must not be empty");
	std::vector<std::string> result;
	std::size_t start = 0;
	while (start <= input.size()) {
		const auto end = input.find(':', start);
		const auto item = input.substr(start,
				end == std::string_view::npos ? input.size() - start : end - start);
		if (item.empty())
			throw std::invalid_argument(
					"CLProtocol libraries must not contain an empty path");
		result.emplace_back(item);
		if (end == std::string_view::npos)
			break;
		start = end + 1;
	}
	return result;
}

} // namespace

const char *output_mode_name(OutputMode mode) noexcept
{
	switch (mode) {
	case OutputMode::frame:
		return "frame";
	case OutputMode::row_block:
		return "row-block";
	}
	return "frame";
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

std::string format_clprotocol_libraries(
		const std::vector<std::string> &libraries)
{
	std::string result;
	for (const auto &library : libraries) {
		if (!result.empty())
			result += ':';
		result += library;
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
	if ((value = spa_dict_lookup(info,
			SPA_KEY_API_EGRABBER_CLPROTOCOL_LIBRARIES)))
		options.clprotocol_libraries = parse_clprotocol_libraries(value);
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_CLPROTOCOL_DEVICE)))
		options.clprotocol_device_template = value;
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_CAMERA_SERIAL)))
		options.camera_serial = value;
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_GENAPI_RUNTIME)))
		options.genapi_runtime = value;
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_CONTROL_TIMEOUT_MS)))
		options.control_timeout_ms = parse_unsigned<std::uint32_t>(value,
				SPA_KEY_API_EGRABBER_CONTROL_TIMEOUT_MS);
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_OUTPUT_MODE)))
		options.output_mode = parse_output_mode(value);
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_ROW_BLOCK_ROWS)))
		options.row_block_rows = parse_unsigned<std::uint32_t>(value,
				SPA_KEY_API_EGRABBER_ROW_BLOCK_ROWS);
	if ((value = spa_dict_lookup(info, SPA_KEY_API_EGRABBER_DETECTOR_PROFILE)))
		options.detector_profile = value;
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
	if (options.row_block_rows == 0)
		throw std::invalid_argument("eGrabber row-block rows must be positive");
	if (options.output_mode == OutputMode::row_block &&
			(!options.detector_profile || options.detector_profile->empty()))
		throw std::invalid_argument(
				"eGrabber row-block mode requires a detector profile");
	if (options.control != "auto" && options.control != "remote" &&
			options.control != "clprotocol" && options.control != "none")
		throw std::invalid_argument("eGrabber control must be auto, remote, clprotocol, or none");
	if (options.control_timeout_ms == 0)
		throw std::invalid_argument("eGrabber control timeout must be greater than zero");
	const bool clprotocol_configuration = !options.clprotocol_libraries.empty() ||
			options.clprotocol_device_template || options.camera_serial ||
			options.genapi_runtime;
	if (clprotocol_configuration && options.control != "auto" &&
			options.control != "clprotocol")
		throw std::invalid_argument(
				"CLProtocol configuration requires eGrabber control auto or clprotocol");
	if (options.acquisition_sequence_context > 3)
		throw std::invalid_argument("acquisition sequence context must be 1, 2, or 3");
	if (options.acquisition_domain.has_value() !=
			(options.acquisition_sequence_context != 0))
		throw std::invalid_argument("acquisition domain and sequence context must be used together");
	if (!options.acquisition_domain && options.acquisition_generation != 0)
		throw std::invalid_argument("acquisition generation requires an acquisition domain");
}

} // namespace egrabber_pipewire
