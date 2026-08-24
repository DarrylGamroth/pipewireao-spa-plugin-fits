/* SPDX-License-Identifier: MIT */

#include "buffer_memory.hpp"

#include <algorithm>

namespace egrabber_pipewire {

AnnouncedMemory choose_announced_memory(
		std::span<const BufferMemoryOffer> offers,
		bool producer_dma_buf_supported)
{
	if (offers.empty())
		return AnnouncedMemory::unavailable;
	if (producer_dma_buf_supported &&
			std::all_of(offers.begin(), offers.end(),
					[](const BufferMemoryOffer &offer) {
						return offer.type == OfferedMemory::dma_buf;
					}))
		return AnnouncedMemory::direct_dma_buf;
	if (std::all_of(offers.begin(), offers.end(),
				[](const BufferMemoryOffer &offer) { return offer.mapped; }))
		return AnnouncedMemory::mapped_host;
	return AnnouncedMemory::unavailable;
}

} // namespace egrabber_pipewire
