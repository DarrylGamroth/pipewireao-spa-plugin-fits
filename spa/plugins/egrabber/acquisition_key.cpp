/* SPDX-License-Identifier: MIT */

#include "acquisition_key.hpp"

#include <limits>
#include <stdexcept>

namespace egrabber_pipewire {

AcquisitionKeySequence::AcquisitionKeySequence(std::uint64_t generation) noexcept
	: generation_(generation)
{
}

void AcquisitionKeySequence::start()
{
	if (started_) {
		if (generation_ == std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("acquisition generation exhausted");
		generation_++;
	}
	started_ = true;
	last_sequence_.reset();
}

AcquisitionKey AcquisitionKeySequence::observe(std::uint32_t sequence)
{
	bool discontinuity = false;
	if (last_sequence_ && sequence <= *last_sequence_) {
		if (generation_ == std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("acquisition generation exhausted");
		generation_++;
		discontinuity = true;
	}
	last_sequence_ = sequence;
	return {generation_, sequence, discontinuity};
}

} // namespace egrabber_pipewire
