/* SPDX-License-Identifier: MIT */

#include "buffer_memory.hpp"

#include <array>

#include <spa/utils/defs.h>

using namespace egrabber_pipewire;

int main()
{
	const std::array<BufferMemoryOffer, 2> mapped = {{
		BufferMemoryOffer{OfferedMemory::mem_ptr, true},
		BufferMemoryOffer{OfferedMemory::mem_fd, true},
	}};
	spa_assert_se(choose_announced_memory(mapped, true) ==
			AnnouncedMemory::mapped_host);

	const std::array<BufferMemoryOffer, 2> dma = {{
		BufferMemoryOffer{OfferedMemory::dma_buf, false},
		BufferMemoryOffer{OfferedMemory::dma_buf, false},
	}};
	spa_assert_se(choose_announced_memory(dma, true) ==
			AnnouncedMemory::direct_dma_buf);
	spa_assert_se(choose_announced_memory(dma, false) ==
			AnnouncedMemory::unavailable);

	const std::array<BufferMemoryOffer, 2> mixed = {{
		BufferMemoryOffer{OfferedMemory::mem_fd, true},
		BufferMemoryOffer{OfferedMemory::dma_buf, false},
	}};
	spa_assert_se(choose_announced_memory(mixed, true) ==
			AnnouncedMemory::unavailable);
	spa_assert_se(choose_announced_memory(
			std::span<const BufferMemoryOffer>{}, true) ==
			AnnouncedMemory::unavailable);
	return 0;
}
