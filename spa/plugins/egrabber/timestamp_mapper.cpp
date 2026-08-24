/* SPDX-License-Identifier: MIT */

#include "timestamp_mapper.hpp"

#include <algorithm>
#include <limits>

namespace egrabber_pipewire {
namespace {

constexpr std::uint64_t maximum_clock_error_ns = 2'000'000'000;

std::int64_t signed_timestamp(std::uint64_t value) noexcept
{
	return static_cast<std::int64_t>(std::min<std::uint64_t>(value,
			static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
}

std::uint64_t distance(std::uint64_t a, std::uint64_t b) noexcept
{
	return a > b ? a - b : b - a;
}

} // namespace

void TimestampMapper::request_reset() noexcept
{
	reset_requested_.store(true, std::memory_order_release);
}

TimestampMapping TimestampMapper::anchor(std::uint64_t camera_ns,
		std::uint64_t monotonic_ns, bool clock_reset) noexcept
{
	initialized_ = true;
	camera_origin_ = camera_ns;
	monotonic_origin_ = monotonic_ns;
	last_camera_ = camera_ns;
	last_pts_ = signed_timestamp(monotonic_ns);
	return {last_pts_, true, clock_reset, camera_ns != 0};
}

TimestampMapping TimestampMapper::map(std::uint64_t camera_ns,
		std::uint64_t monotonic_ns) noexcept
{
	const bool requested_reset = reset_requested_.exchange(false,
			std::memory_order_acq_rel);
	if (requested_reset || !initialized_)
		return anchor(camera_ns, monotonic_ns, false);

	if (camera_ns == 0) {
		auto pts = signed_timestamp(monotonic_ns);
		bool reset = false;
		if (pts <= last_pts_) {
			pts = last_pts_ == std::numeric_limits<std::int64_t>::max()
				? last_pts_ : last_pts_ + 1;
			reset = true;
		}
		last_pts_ = pts;
		last_camera_ = 0;
		return {pts, reset, reset, false};
	}

	if (last_camera_ == 0 || camera_ns <= last_camera_)
		return anchor(camera_ns, monotonic_ns, true);

	const auto camera_elapsed = camera_ns - camera_origin_;
	if (camera_elapsed > std::numeric_limits<std::uint64_t>::max() -
			monotonic_origin_)
		return anchor(camera_ns, monotonic_ns, true);
	const auto mapped = monotonic_origin_ + camera_elapsed;
	const auto pts = signed_timestamp(mapped);
	if (pts <= last_pts_ ||
			distance(mapped, monotonic_ns) > maximum_clock_error_ns)
		return anchor(camera_ns, monotonic_ns, true);

	last_camera_ = camera_ns;
	last_pts_ = pts;
	return {pts, false, false, true};
}

} // namespace egrabber_pipewire
