/* SPDX-License-Identifier: MIT */

#pragma once

#include <cstdint>
#include <optional>

namespace egrabber_pipewire {

struct AcquisitionKey {
	std::uint64_t generation = 0;
	std::uint64_t sequence = 0;
};

class AcquisitionKeySequence {
public:
	explicit AcquisitionKeySequence(std::uint64_t generation = 0) noexcept;
	AcquisitionKey observe(std::uint32_t sequence);

private:
	std::uint64_t generation_ = 0;
	std::optional<std::uint32_t> last_sequence_;
	bool invalid_ = false;
};

} // namespace egrabber_pipewire
