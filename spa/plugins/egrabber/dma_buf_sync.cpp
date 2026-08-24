/* SPDX-License-Identifier: MIT */

#include "dma_buf_sync.hpp"

#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>

#include <xf86drm.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace egrabber_pipewire {
namespace {

std::runtime_error drm_error(const char *operation)
{
	return std::runtime_error(std::string(operation) + ": " +
			std::strerror(errno));
}

} // namespace

DmaBufTimeline::DmaBufTimeline(int drm_fd, std::uint32_t acquire_handle,
		std::uint32_t release_handle,
		struct spa_meta_sync_timeline *metadata) noexcept
	: drm_fd_(drm_fd), acquire_handle_(acquire_handle),
	  release_handle_(release_handle), metadata_(metadata)
{
}

DmaBufTimeline::~DmaBufTimeline()
{
	reset();
}

DmaBufTimeline::DmaBufTimeline(DmaBufTimeline &&other) noexcept
	: drm_fd_(other.drm_fd_), acquire_handle_(other.acquire_handle_),
	  release_handle_(other.release_handle_), metadata_(other.metadata_)
{
	other.drm_fd_ = -1;
	other.acquire_handle_ = 0;
	other.release_handle_ = 0;
	other.metadata_ = nullptr;
}

DmaBufTimeline &DmaBufTimeline::operator=(DmaBufTimeline &&other) noexcept
{
	if (this == &other)
		return *this;
	reset();
	drm_fd_ = other.drm_fd_;
	acquire_handle_ = other.acquire_handle_;
	release_handle_ = other.release_handle_;
	metadata_ = other.metadata_;
	other.drm_fd_ = -1;
	other.acquire_handle_ = 0;
	other.release_handle_ = 0;
	other.metadata_ = nullptr;
	return *this;
}

void DmaBufTimeline::reset() noexcept
{
	if (drm_fd_ >= 0 && acquire_handle_ != 0)
		(void) drmSyncobjDestroy(drm_fd_, acquire_handle_);
	if (drm_fd_ >= 0 && release_handle_ != 0)
		(void) drmSyncobjDestroy(drm_fd_, release_handle_);
	drm_fd_ = -1;
	acquire_handle_ = 0;
	release_handle_ = 0;
	metadata_ = nullptr;
}

bool DmaBufTimeline::release_ready()
{
	if (metadata_ == nullptr || metadata_->release_point == 0 ||
			SPA_FLAG_IS_SET(metadata_->flags,
					SPA_META_SYNC_TIMELINE_UNSCHEDULED_RELEASE))
		return true;
	auto handle = release_handle_;
	auto point = metadata_->release_point;
	uint64_t signaled = 0;
	if (drmSyncobjQuery(drm_fd_, &handle, &signaled, 1) != 0)
		throw drm_error("querying the DMA-BUF release timeline failed");
	return signaled >= point;
}

void DmaBufTimeline::signal_acquire(std::uint64_t point)
{
	if (metadata_ == nullptr)
		return;
	SPA_FLAG_SET(metadata_->flags,
			SPA_META_SYNC_TIMELINE_UNSCHEDULED_RELEASE);
	metadata_->acquire_point = point;
	metadata_->release_point = point;
	auto handle = acquire_handle_;
	if (drmSyncobjTimelineSignal(drm_fd_, &handle, &point, 1) != 0)
		throw drm_error("signaling the DMA-BUF acquire timeline failed");
}

DmaBufSyncContext::DmaBufSyncContext()
{
	for (unsigned int index = 128; index < 144; ++index) {
		const std::string candidate = "/dev/dri/renderD" +
				std::to_string(index);
		const int fd = open(candidate.c_str(), O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		std::uint64_t syncobj = 0;
		std::uint64_t timeline = 0;
		if (drmGetCap(fd, DRM_CAP_SYNCOBJ, &syncobj) == 0 && syncobj != 0 &&
				drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &timeline) == 0 &&
				timeline != 0) {
			drm_fd_ = fd;
			device_ = candidate;
			return;
		}
		(void) close(fd);
	}
	unavailable_reason_ =
			"no accessible DRM render node supports SyncObj timelines";
}

DmaBufSyncContext::~DmaBufSyncContext()
{
	if (drm_fd_ >= 0)
		(void) close(drm_fd_);
}

DmaBufTimeline DmaBufSyncContext::import(struct spa_buffer *buffer) const
{
	auto *metadata = static_cast<struct spa_meta_sync_timeline *>(
			spa_buffer_find_meta_data(buffer, SPA_META_SyncTimeline,
					sizeof(struct spa_meta_sync_timeline)));
	if (metadata == nullptr)
		throw std::runtime_error("DMA-BUF is missing SyncTimeline metadata");
	if (drm_fd_ < 0)
		throw std::runtime_error("explicit DMA-BUF synchronization is unavailable: " +
				unavailable_reason_);
	if (buffer->n_datas < 3)
		throw std::runtime_error(
				"SyncTimeline metadata is missing its SyncObj data blocks");
	const auto &acquire = buffer->datas[buffer->n_datas - 2];
	const auto &release = buffer->datas[buffer->n_datas - 1];
	if (acquire.type != SPA_DATA_SyncObj ||
			release.type != SPA_DATA_SyncObj || acquire.fd < 0 || release.fd < 0)
		throw std::runtime_error("invalid DMA-BUF SyncObj descriptors");

	std::uint32_t acquire_handle = 0;
	std::uint32_t release_handle = 0;
	if (drmSyncobjFDToHandle(drm_fd_, acquire.fd, &acquire_handle) != 0)
		throw drm_error("importing the DMA-BUF acquire timeline failed");
	if (drmSyncobjFDToHandle(drm_fd_, release.fd, &release_handle) != 0) {
		(void) drmSyncobjDestroy(drm_fd_, acquire_handle);
		throw drm_error("importing the DMA-BUF release timeline failed");
	}
	return DmaBufTimeline(drm_fd_, acquire_handle, release_handle, metadata);
}

} // namespace egrabber_pipewire
