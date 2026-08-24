/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "frame_sequence.hpp"

#include <optional>

#include <spa/utils/defs.h>

using egrabber_pipewire::FrameSequence;

int main()
{
	FrameSequence sequence;
	auto result = sequence.next(100);
	spa_assert_se(result.sequence == 0 && !result.discontinuity && result.lost == 0);
	result = sequence.next(101);
	spa_assert_se(result.sequence == 1 && !result.discontinuity && result.lost == 0);
	result = sequence.next(104);
	spa_assert_se(result.sequence == 4 && result.discontinuity && result.lost == 2);

	sequence.reset();
	result = sequence.next(500);
	spa_assert_se(result.sequence == 5 && !result.discontinuity && result.lost == 0);
	result = sequence.next(500);
	spa_assert_se(result.sequence == 6 && result.discontinuity && result.lost == 0);

	FrameSequence wrapped;
	result = wrapped.next(65535);
	result = wrapped.next(1);
	spa_assert_se(result.sequence == 1 && !result.discontinuity && result.lost == 0);
	result = wrapped.next(4);
	spa_assert_se(result.sequence == 4 && result.discontinuity && result.lost == 2);

	FrameSequence unavailable;
	result = unavailable.next(std::nullopt);
	result = unavailable.next(std::nullopt);
	spa_assert_se(result.sequence == 1 && !result.discontinuity && result.lost == 0);
	return 0;
}
