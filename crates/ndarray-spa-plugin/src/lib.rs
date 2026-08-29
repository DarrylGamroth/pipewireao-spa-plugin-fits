//! Generic loadable PipeWireAO ndarray transforms.

#![allow(unsafe_code)]

#[cfg(not(target_endian = "little"))]
compile_error!("the current ndarray SPA payload adapter requires a little-endian target");

mod config;
mod frame_assembly;
mod video_view;

use std::ptr;

use pipewireao_spa_node::{Factory, sys};

pub use frame_assembly::FRAME_ASSEMBLY_FACTORY_NAME;
pub use video_view::VIDEO_VIEW_FACTORY_NAME;

static FACTORIES: [&Factory; 2] = [&frame_assembly::FACTORY, &video_view::FACTORY];

/// Enumerates the generic ndarray SPA factories in this shared object.
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
            for expected in FACTORIES {
                assert_eq!(spa_handle_factory_enum(&mut factory, &mut index), 1);
                assert_eq!(factory, expected.as_ptr());
            }
            assert_eq!(spa_handle_factory_enum(&mut factory, &mut index), 0);
        }
    }
}
