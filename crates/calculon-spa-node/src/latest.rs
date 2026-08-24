//! Narrow C bridge to PipeWireAO's header-only latest-buffer transport.

use std::ffi::c_void;
use std::ptr;

use libspa::sys;

unsafe extern "C" {
    fn calculon_spa_unwrap_fixed_pod(
        source: *const sys::spa_pod,
        storage: *mut c_void,
        size: usize,
    ) -> usize;
    fn calculon_spa_latest_new(direction: u32, data: *mut c_void) -> *mut c_void;
    fn calculon_spa_latest_destroy(latest: *mut c_void);
    fn calculon_spa_latest_set_buffers(
        latest: *mut c_void,
        buffers: *mut *mut sys::spa_buffer,
        n_buffers: u32,
    );
    fn calculon_spa_latest_clear_buffers(latest: *mut c_void);
    fn calculon_spa_latest_set_io(
        latest: *mut c_void,
        id: u32,
        data: *mut c_void,
        size: usize,
    ) -> i32;
    fn calculon_spa_latest_has_links(latest: *const c_void) -> bool;
    fn calculon_spa_latest_worker_is_active(latest: *const c_void) -> bool;
    fn calculon_spa_latest_worker_begin(latest: *mut c_void) -> i32;
    fn calculon_spa_latest_worker_end(latest: *mut c_void) -> i32;
    fn calculon_spa_latest_dequeue(latest: *mut c_void, buffer_id: *mut u32) -> i32;
    fn calculon_spa_latest_queue(latest: *mut c_void, buffer_id: u32) -> i32;
    fn calculon_spa_latest_return(latest: *mut c_void, buffer_id: u32) -> i32;
}

pub(crate) unsafe fn unwrap_fixed_pod(pod: *const sys::spa_pod) -> Result<Vec<u64>, i32> {
    let pod = unsafe { pod.as_ref() }.ok_or(-libc::EINVAL)?;
    let source_size = std::mem::size_of::<sys::spa_pod>()
        .checked_add(pod.size as usize)
        .ok_or(-libc::EOVERFLOW)?;
    let mut storage = vec![0_u64; source_size.div_ceil(std::mem::size_of::<u64>())];
    let size = unsafe {
        calculon_spa_unwrap_fixed_pod(
            pod,
            storage.as_mut_ptr().cast(),
            storage.len() * std::mem::size_of::<u64>(),
        )
    };
    if size == 0 || size > storage.len() * std::mem::size_of::<u64>() {
        return Err(-libc::EINVAL);
    }
    storage.truncate(size.div_ceil(std::mem::size_of::<u64>()));
    Ok(storage)
}

pub(crate) struct LatestBuffers {
    state: *mut c_void,
}

impl LatestBuffers {
    pub(crate) const fn empty() -> Self {
        Self {
            state: ptr::null_mut(),
        }
    }

    fn ensure(&mut self, direction: u32, owner: *mut c_void) -> Result<bool, i32> {
        if self.state.is_null() {
            self.state = unsafe { calculon_spa_latest_new(direction, owner) };
            if self.state.is_null() {
                return Err(-libc::ENOMEM);
            }
            return Ok(true);
        }
        Ok(false)
    }

    pub(crate) fn set_io(
        &mut self,
        direction: u32,
        owner: *mut c_void,
        id: u32,
        data: *mut c_void,
        size: usize,
        buffers: &mut [*mut sys::spa_buffer],
    ) -> Result<(), i32> {
        if self.ensure(direction, owner)? {
            unsafe {
                calculon_spa_latest_set_buffers(
                    self.state,
                    buffers.as_mut_ptr(),
                    u32::try_from(buffers.len()).map_err(|_| -libc::EOVERFLOW)?,
                );
            }
        }
        let result = unsafe { calculon_spa_latest_set_io(self.state, id, data, size) };
        if result < 0 { Err(result) } else { Ok(()) }
    }

    pub(crate) fn set_buffers(&mut self, buffers: &mut [*mut sys::spa_buffer]) {
        if !self.state.is_null() {
            unsafe {
                calculon_spa_latest_set_buffers(
                    self.state,
                    buffers.as_mut_ptr(),
                    buffers.len() as u32,
                );
            }
        }
    }

    pub(crate) fn clear_buffers(&mut self) {
        if !self.state.is_null() {
            unsafe { calculon_spa_latest_clear_buffers(self.state) };
        }
    }

    pub(crate) fn has_links(&self) -> bool {
        !self.state.is_null() && unsafe { calculon_spa_latest_has_links(self.state) }
    }

    pub(crate) fn worker_begin(&mut self) -> Result<(), i32> {
        if !self.has_links() || self.worker_is_active() {
            return Ok(());
        }
        let result = unsafe { calculon_spa_latest_worker_begin(self.state) };
        if result < 0 { Err(result) } else { Ok(()) }
    }

    pub(crate) fn worker_end(&mut self) -> Result<(), i32> {
        if !self.worker_is_active() {
            return Ok(());
        }
        let result = unsafe { calculon_spa_latest_worker_end(self.state) };
        if result < 0 { Err(result) } else { Ok(()) }
    }

    fn worker_is_active(&self) -> bool {
        !self.state.is_null() && unsafe { calculon_spa_latest_worker_is_active(self.state) }
    }

    pub(crate) fn dequeue(&mut self) -> Result<Option<u32>, i32> {
        let mut id = sys::SPA_ID_INVALID;
        let result = unsafe { calculon_spa_latest_dequeue(self.state, &mut id) };
        match result {
            1 => Ok(Some(id)),
            0 => Ok(None),
            error if error < 0 => Err(error),
            _ => Err(-libc::EPROTO),
        }
    }

    pub(crate) fn queue(&mut self, id: u32) -> Result<(), i32> {
        let result = unsafe { calculon_spa_latest_queue(self.state, id) };
        if result < 0 { Err(result) } else { Ok(()) }
    }

    pub(crate) fn return_buffer(&mut self, id: u32) -> Result<(), i32> {
        let result = unsafe { calculon_spa_latest_return(self.state, id) };
        if result < 0 { Err(result) } else { Ok(()) }
    }
}

impl Drop for LatestBuffers {
    fn drop(&mut self) {
        if !self.state.is_null() {
            unsafe { calculon_spa_latest_destroy(self.state) };
        }
    }
}

unsafe impl Send for LatestBuffers {}
