//! Loadable PipeWireAO SPA processing nodes for Calculon algorithms.

#![allow(unsafe_code)]

#[cfg(not(target_endian = "little"))]
compile_error!("the current Calculon SPA payload adapters require a little-endian target");

mod pixel_calibration;

use std::ptr;

use calculon_spa_node::{Factory, sys};

pub use pixel_calibration::PIXEL_CALIBRATION_FACTORY_NAME;

static FACTORIES: [&Factory; 1] = [&pixel_calibration::FACTORY];

/// Enumerates the Calculon SPA factories in this shared object.
///
/// # Safety
///
/// `factory` and `index` must be non-null writable pointers supplied by a SPA
/// host. Returned factory pointers are immutable and process-static.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn spa_handle_factory_enum(
    factory: *mut *const sys::spa_handle_factory,
    index: *mut u32,
) -> i32 {
    if factory.is_null() || index.is_null() {
        return -libc::EINVAL;
    }
    let position = unsafe { *index as usize };
    let Some(next) = FACTORIES.get(position) else {
        return 0;
    };
    unsafe {
        ptr::write(factory, next.as_ptr());
        *index += 1;
    }
    1
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_factory_is_enumerated_once() {
        unsafe {
            let mut factory = ptr::null();
            let mut index = 0;
            assert_eq!(spa_handle_factory_enum(&mut factory, &mut index), 1);
            assert!(!factory.is_null());
            assert_eq!(spa_handle_factory_enum(&mut factory, &mut index), 0);
        }
    }
}
