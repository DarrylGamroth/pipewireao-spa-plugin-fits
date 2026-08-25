/* SPDX-License-Identifier: MIT */

#include "acquisition_key.hpp"

#include <stdexcept>

namespace egrabber_pipewire {

AcquisitionKeySequence::AcquisitionKeySequence(std::uint64_t generation) noexcept
	: generation_(generation)
{
}

AcquisitionKey AcquisitionKeySequence::observe(std::uint32_t sequence)
{
	if (last_sequence_ && sequence <= *last_sequence_)
		throw std::runtime_error(
				"non-increasing acquisition sequence requires a new shared generation");
	last_sequence_ = sequence;
	return {generation_, sequence};
}

} // namespace egrabber_pipewire
