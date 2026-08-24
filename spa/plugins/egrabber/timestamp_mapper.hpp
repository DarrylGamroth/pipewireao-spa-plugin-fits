/* SPDX-License-Identifier: MIT */

#pragma once

#include <atomic>
#include <cstdint>

namespace egrabber_pipewire {

struct TimestampMapping {
	std::int64_t pts = 0;
	bool discontinuity = false;
	bool clock_reset = false;
	bool used_camera_clock = false;
};

class TimestampMapper {
public:
	void request_reset() noexcept;
	TimestampMapping map(std::uint64_t camera_ns,
			std::uint64_t monotonic_ns) noexcept;

private:
	TimestampMapping anchor(std::uint64_t camera_ns,
			std::uint64_t monotonic_ns, bool clock_reset) noexcept;

	std::atomic<bool> reset_requested_{true};
	bool initialized_ = false;
	std::uint64_t camera_origin_ = 0;
	std::uint64_t monotonic_origin_ = 0;
	std::uint64_t last_camera_ = 0;
	std::int64_t last_pts_ = -1;
};

} // namespace egrabber_pipewire
