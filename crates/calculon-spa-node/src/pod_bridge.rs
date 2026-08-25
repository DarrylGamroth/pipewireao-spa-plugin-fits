//! Narrow C bridge for fixed-choice POD normalization.

use std::ffi::c_void;

use libspa::sys;

unsafe extern "C" {
    fn calculon_spa_unwrap_fixed_pod(
        source: *const sys::spa_pod,
        storage: *mut c_void,
        size: usize,
    ) -> usize;
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
