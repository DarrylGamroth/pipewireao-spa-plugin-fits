/* SPDX-License-Identifier: MIT */

#include "acquisition_key.hpp"

#include <cassert>
#include <stdexcept>

using egrabber_pipewire::AcquisitionKeySequence;

int main()
{
	AcquisitionKeySequence sequence(42);
	auto key = sequence.observe(100);
	assert(key.generation == 42 && key.sequence == 100);
	key = sequence.observe(101);
	assert(key.generation == 42 && key.sequence == 101);
	bool threw = false;
	try {
		(void) sequence.observe(0);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	assert(threw);
	threw = false;
	try {
		(void) sequence.observe(102);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	assert(threw);

	AcquisitionKeySequence duplicate(42);
	(void) duplicate.observe(100);
	threw = false;
	try {
		(void) duplicate.observe(100);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	assert(threw);
}
