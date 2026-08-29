//! Loadable PipeWireAO decoder for the Nüvü HNü240 Camera Link carrier.

#![allow(unsafe_code)]

#[cfg(not(target_endian = "little"))]
compile_error!("the HNü240 SPA decoder requires a little-endian target");

mod config;
mod hnu240_decoder;

use std::ptr;

use pipewireao_spa_node::{Factory, sys};

pub use hnu240_decoder::{HNU240_CL_FULL_PROFILE, HNU240_DECODER_FACTORY_NAME};

static FACTORIES: [&Factory; 1] = [&hnu240_decoder::FACTORY];

/// Enumerates the Nüvü SPA factories in this shared object.
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
    fn decoder_factory_is_enumerated_once() {
        unsafe {
            let mut factory = ptr::null();
            let mut index = 0;
            assert_eq!(spa_handle_factory_enum(&mut factory, &mut index), 1);
            assert_eq!(factory, FACTORIES[0].as_ptr());
            assert_eq!(spa_handle_factory_enum(&mut factory, &mut index), 0);
        }
    }
}
