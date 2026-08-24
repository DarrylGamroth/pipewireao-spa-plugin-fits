/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "dma_buf_sync.hpp"

#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/utils/defs.h>

#include <xf86drm.h>

#include <fcntl.h>
#include <unistd.h>

int main()
{
	egrabber_pipewire::DmaBufTimeline empty;
	spa_assert_se(empty.release_ready());

	egrabber_pipewire::DmaBufSyncContext context;
	if (!context.available())
		return 77;
	const int drm_fd = open(context.device().c_str(), O_RDWR | O_CLOEXEC);
	spa_assert_se(drm_fd >= 0);

	uint32_t acquire_handle = 0;
	uint32_t release_handle = 0;
	spa_assert_se(drmSyncobjCreate(drm_fd, 0, &acquire_handle) == 0);
	spa_assert_se(drmSyncobjCreate(drm_fd, 0, &release_handle) == 0);
	int acquire_fd = -1;
	int release_fd = -1;
	spa_assert_se(drmSyncobjHandleToFD(drm_fd, acquire_handle, &acquire_fd) == 0);
	spa_assert_se(drmSyncobjHandleToFD(drm_fd, release_handle, &release_fd) == 0);

	struct spa_data datas[3] = {};
	datas[1].type = SPA_DATA_SyncObj;
	datas[1].fd = acquire_fd;
	datas[2].type = SPA_DATA_SyncObj;
	datas[2].fd = release_fd;
	struct spa_meta_sync_timeline metadata = {};
	struct spa_meta meta = {
		.type = SPA_META_SyncTimeline,
		.size = sizeof(metadata),
		.data = &metadata,
	};
	struct spa_buffer buffer = {
		.n_metas = 1,
		.n_datas = SPA_N_ELEMENTS(datas),
		.metas = &meta,
		.datas = datas,
	};
	{
		auto timeline = context.import(&buffer);
		metadata.release_point = 1;
		spa_assert_se(!timeline.release_ready());
		uint64_t point = metadata.release_point;
		spa_assert_se(drmSyncobjTimelineSignal(drm_fd, &release_handle,
				&point, 1) == 0);
		spa_assert_se(timeline.release_ready());
		metadata.release_point = 2;
		metadata.flags = SPA_META_SYNC_TIMELINE_UNSCHEDULED_RELEASE;
		spa_assert_se(timeline.release_ready());
	}

	spa_assert_se(close(acquire_fd) == 0);
	spa_assert_se(close(release_fd) == 0);
	spa_assert_se(drmSyncobjDestroy(drm_fd, acquire_handle) == 0);
	spa_assert_se(drmSyncobjDestroy(drm_fd, release_handle) == 0);
	spa_assert_se(close(drm_fd) == 0);
	return 0;
}
