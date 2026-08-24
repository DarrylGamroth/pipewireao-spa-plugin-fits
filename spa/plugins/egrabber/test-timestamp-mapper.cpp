/* SPDX-License-Identifier: MIT */

#include "timestamp_mapper.hpp"

#include <cassert>

using egrabber_pipewire::TimestampMapper;

int main()
{
	TimestampMapper mapper;

	auto first = mapper.map(10'000, 1'000'000);
	assert(first.pts == 1'000'000);
	assert(first.discontinuity);
	assert(!first.clock_reset);
	assert(first.used_camera_clock);

	auto second = mapper.map(20'000, 1'012'000);
	assert(second.pts == 1'010'000);
	assert(!second.discontinuity);

	auto backwards = mapper.map(5'000, 2'000'000);
	assert(backwards.pts == 2'000'000);
	assert(backwards.discontinuity);
	assert(backwards.clock_reset);

	mapper.request_reset();
	auto requested = mapper.map(6'000, 3'000'000);
	assert(requested.pts == 3'000'000);
	assert(requested.discontinuity);
	assert(!requested.clock_reset);

	TimestampMapper fallback;
	auto no_camera_clock = fallback.map(0, 100);
	assert(no_camera_clock.pts == 100);
	assert(!no_camera_clock.used_camera_clock);
	auto monotonic = fallback.map(0, 100);
	assert(monotonic.pts == 101);
	assert(monotonic.clock_reset);
}
