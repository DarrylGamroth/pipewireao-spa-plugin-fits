//! Loadable PipeWireAO SPA processing nodes for Calculon algorithms.

#![allow(unsafe_code)]

#[cfg(not(target_endian = "little"))]
compile_error!("the current Calculon SPA payload adapters require a little-endian target");

mod alpao_command_normalization;
mod config;
mod frame_assembly;
mod pixel_calibration;
mod shwfs_controller;

use std::ptr;

use calculon_spa_node::{Factory, sys};

pub use alpao_command_normalization::{
    ALPAO_COMMAND_NORMALIZATION_FACTORY_NAME, ALPAO_NORMALIZED_ACTUATOR_COMMAND_V1,
};
pub use frame_assembly::FRAME_ASSEMBLY_FACTORY_NAME;
pub use pixel_calibration::PIXEL_CALIBRATION_FACTORY_NAME;
pub use shwfs_controller::SHWFS_CONTROLLER_FACTORY_NAME;

/// Schema for one complete immutable block of calibrated detector rows.
pub const CALIBRATED_PIXEL_ROW_BLOCK_V1: &str = "org.calculon.ao.calibrated-pixel-row-block/1";

/// Schema for one complete immutable block of raw detector rows.
pub const RAW_PIXEL_ROW_BLOCK_V1: &str = "org.calculon.ao.raw-pixel-row-block/1";

static FACTORIES: [&Factory; 4] = [
    &pixel_calibration::FACTORY,
    &frame_assembly::FACTORY,
    &shwfs_controller::FACTORY,
    &alpao_command_normalization::FACTORY,
];

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
            for expected in FACTORIES {
                assert_eq!(spa_handle_factory_enum(&mut factory, &mut index), 1);
                assert_eq!(factory, expected.as_ptr());
            }
            assert_eq!(spa_handle_factory_enum(&mut factory, &mut index), 0);
        }
    }
}
