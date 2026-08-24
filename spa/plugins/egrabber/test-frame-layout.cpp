/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "frame_layout.hpp"

#include <stdexcept>

#include <spa/utils/defs.h>

using namespace egrabber_pipewire;

namespace {

NegotiatedFrameLayout negotiated()
{
	return {4, 3, 2, 1, 4, 32, "Mono8"};
}

DeliveredFrameLayout delivered()
{
	DeliveredFrameLayout result;
	result.width = 4;
	result.delivered_height = 3;
	result.line_pitch = 4;
	result.pixel_format = "Mono8";
	result.offset_x = 2;
	result.offset_y = 1;
	result.image_offset = 0;
	result.size_filled = 12;
	result.data_size = 12;
	result.image_present = true;
	result.data_larger_than_buffer = false;
	return result;
}

} // namespace

int main()
{
	spa_assert_se(committed_prefix(0, 24, 8) == 0);
	spa_assert_se(committed_prefix(7, 24, 8) == 0);
	spa_assert_se(committed_prefix(17, 24, 8) == 16);
	spa_assert_se(committed_prefix(24, 24, 8) == 24);
	spa_assert_se(committed_prefix(30, 24, 8) == 24);

	auto frame = resolve_frame_layout(negotiated(), delivered());
	spa_assert_se(frame.image_offset == 0 && frame.data_size == 12);
	spa_assert_se(frame.line_pitch == 4 && !frame.incomplete && !frame.corrupted);

	auto chunks = delivered();
	chunks.image_offset = 4;
	chunks.data_size = 24;
	chunks.size_filled = 24;
	frame = resolve_frame_layout(negotiated(), chunks);
	spa_assert_se(frame.image_offset == 4 && frame.data_size == 12 && !frame.corrupted);

	auto short_frame = delivered();
	short_frame.size_filled = 9;
	frame = resolve_frame_layout(negotiated(), short_frame);
	spa_assert_se(frame.data_size == 9 && frame.corrupted && !frame.incomplete);

	auto incomplete = delivered();
	incomplete.delivered_height = 2;
	incomplete.size_filled = 8;
	incomplete.data_size = 8;
	frame = resolve_frame_layout(negotiated(), incomplete);
	spa_assert_se(frame.data_size == 8 && frame.corrupted && frame.incomplete);

	auto padded = delivered();
	padded.line_pitch = 6;
	padded.x_padding = 2;
	padded.size_filled = 18;
	padded.data_size = 18;
	frame = resolve_frame_layout(negotiated(), padded);
	spa_assert_se(frame.line_pitch == 6 && frame.data_size == 18 && !frame.corrupted);

	auto absent = delivered();
	absent.image_present = false;
	frame = resolve_frame_layout(negotiated(), absent);
	spa_assert_se(frame.data_size == 0 && frame.corrupted);

	auto overflow = delivered();
	overflow.data_larger_than_buffer = true;
	frame = resolve_frame_layout(negotiated(), overflow);
	spa_assert_se(frame.corrupted);

	auto unsupported = delivered();
	unsupported.payload_type_supported = false;
	frame = resolve_frame_layout(negotiated(), unsupported);
	spa_assert_se(frame.data_size == 0 && frame.corrupted);

	auto changed = delivered();
	changed.width = 5;
	bool rejected = false;
	try {
		(void) resolve_frame_layout(negotiated(), changed);
	} catch (const std::runtime_error &) {
		rejected = true;
	}
	spa_assert_se(rejected);
	return 0;
}
