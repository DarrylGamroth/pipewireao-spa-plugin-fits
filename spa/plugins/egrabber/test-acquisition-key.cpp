/* SPDX-License-Identifier: MIT */

#include "acquisition_key.hpp"

#include <cassert>
#include <limits>
#include <stdexcept>

using egrabber_pipewire::AcquisitionKeySequence;

int main()
{
	AcquisitionKeySequence sequence(42);
	sequence.start();
	auto key = sequence.observe(100);
	assert(key.generation == 42 && key.sequence == 100 && !key.discontinuity);
	key = sequence.observe(101);
	assert(key.generation == 42 && key.sequence == 101 && !key.discontinuity);
	key = sequence.observe(0);
	assert(key.generation == 43 && key.sequence == 0 && key.discontinuity);

	sequence.start();
	key = sequence.observe(7);
	assert(key.generation == 44 && key.sequence == 7 && !key.discontinuity);

	AcquisitionKeySequence exhausted(std::numeric_limits<std::uint64_t>::max());
	exhausted.start();
	assert(exhausted.observe(1).generation ==
			std::numeric_limits<std::uint64_t>::max());
	bool threw = false;
	try {
		exhausted.start();
	} catch (const std::overflow_error &) {
		threw = true;
	}
	assert(threw);
}
