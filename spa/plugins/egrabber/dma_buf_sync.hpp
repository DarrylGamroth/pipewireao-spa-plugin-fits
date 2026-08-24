/* SPDX-License-Identifier: MIT */

#pragma once

#include <cstdint>
#include <string>

struct spa_buffer;
struct spa_meta_sync_timeline;

namespace egrabber_pipewire {

class DmaBufSyncContext;

class DmaBufTimeline {
public:
	DmaBufTimeline() = default;
	~DmaBufTimeline();
	DmaBufTimeline(DmaBufTimeline &&other) noexcept;
	DmaBufTimeline &operator=(DmaBufTimeline &&other) noexcept;
	DmaBufTimeline(const DmaBufTimeline &) = delete;
	DmaBufTimeline &operator=(const DmaBufTimeline &) = delete;

	bool release_ready();
	void signal_acquire(std::uint64_t point);
	explicit operator bool() const noexcept { return metadata_ != nullptr; }

private:
	friend class DmaBufSyncContext;
	DmaBufTimeline(int drm_fd, std::uint32_t acquire_handle,
			std::uint32_t release_handle,
			struct spa_meta_sync_timeline *metadata) noexcept;
	void reset() noexcept;

	int drm_fd_ = -1;
	std::uint32_t acquire_handle_ = 0;
	std::uint32_t release_handle_ = 0;
	struct spa_meta_sync_timeline *metadata_ = nullptr;
};

class DmaBufSyncContext {
public:
	DmaBufSyncContext();
	~DmaBufSyncContext();
	DmaBufSyncContext(const DmaBufSyncContext &) = delete;
	DmaBufSyncContext &operator=(const DmaBufSyncContext &) = delete;

	bool available() const noexcept { return drm_fd_ >= 0; }
	const std::string &device() const noexcept { return device_; }
	const std::string &unavailable_reason() const noexcept
	{
		return unavailable_reason_;
	}
	DmaBufTimeline import(struct spa_buffer *buffer) const;

private:
	int drm_fd_ = -1;
	std::string device_;
	std::string unavailable_reason_;
};

} // namespace egrabber_pipewire
