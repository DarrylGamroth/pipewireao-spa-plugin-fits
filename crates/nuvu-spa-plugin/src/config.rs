//! Strict parsing of immutable SPA factory construction information.

use std::ffi::{CStr, c_char};

use pipewireao_spa_node::{Rate, sys};

pub(crate) fn required_info<'a>(
    info: Option<&'a sys::spa_dict>,
    key: &[u8],
) -> Result<&'a str, i32> {
    let key = CStr::from_bytes_with_nul(key).map_err(|_| -libc::EINVAL)?;
    let info = info.ok_or(-libc::EINVAL)?;
    if info.n_items != 0 && info.items.is_null() {
        return Err(-libc::EINVAL);
    }
    let items = unsafe { std::slice::from_raw_parts(info.items, info.n_items as usize) };
    for item in items {
        if item.key.is_null() || item.value.is_null() {
            continue;
        }
        let item_key = unsafe { CStr::from_ptr(item.key.cast::<c_char>()) };
        if item_key == key {
            return unsafe { CStr::from_ptr(item.value.cast::<c_char>()) }
                .to_str()
                .map_err(|_| -libc::EINVAL);
        }
    }
    Err(-libc::EINVAL)
}

pub(crate) fn parse_rate(value: &str) -> Result<Rate, i32> {
    let (num, denom) = value.split_once('/').ok_or(-libc::EINVAL)?;
    Rate::new(
        num.parse::<u32>().map_err(|_| -libc::EINVAL)?,
        denom.parse::<u32>().map_err(|_| -libc::EINVAL)?,
    )
}
