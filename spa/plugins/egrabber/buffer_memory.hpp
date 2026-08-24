/* SPDX-License-Identifier: MIT */

#pragma once

#include <span>

namespace egrabber_pipewire {

enum class OfferedMemory { mem_ptr, mem_fd, dma_buf };
enum class AnnouncedMemory { mapped_host, direct_dma_buf, unavailable };

struct BufferMemoryOffer {
	OfferedMemory type = OfferedMemory::mem_ptr;
	bool mapped = false;
};

AnnouncedMemory choose_announced_memory(
		std::span<const BufferMemoryOffer> offers,
		bool producer_dma_buf_supported);

} // namespace egrabber_pipewire
