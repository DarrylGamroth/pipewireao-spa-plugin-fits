//! Strict parsing of immutable SPA factory construction information.

use std::ffi::{CStr, c_char};

use calculon_spa_node::{Rate, sys};

const PROFILE_DIGEST_CHARACTERS: usize = 64;

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

pub(crate) fn parse_size(value: &str) -> Result<(u32, u32), i32> {
    let (width, height) = value.split_once('x').ok_or(-libc::EINVAL)?;
    let width = width.parse::<u32>().map_err(|_| -libc::EINVAL)?;
    let height = height.parse::<u32>().map_err(|_| -libc::EINVAL)?;
    if width == 0 || height == 0 {
        return Err(-libc::EINVAL);
    }
    Ok((width, height))
}

pub(crate) fn parse_rate(value: &str) -> Result<Rate, i32> {
    let (num, denom) = value.split_once('/').ok_or(-libc::EINVAL)?;
    Rate::new(
        num.parse::<u32>().map_err(|_| -libc::EINVAL)?,
        denom.parse::<u32>().map_err(|_| -libc::EINVAL)?,
    )
}

pub(crate) fn parse_positive_usize(value: &str) -> Result<usize, i32> {
    let value = value.parse::<usize>().map_err(|_| -libc::EINVAL)?;
    (value != 0).then_some(value).ok_or(-libc::EINVAL)
}

pub(crate) fn parse_finite_f32(value: &str) -> Result<f32, i32> {
    let value = value.parse::<f32>().map_err(|_| -libc::EINVAL)?;
    value.is_finite().then_some(value).ok_or(-libc::EINVAL)
}

pub(crate) fn valid_profile(profile: &str) -> bool {
    let Some(digest) = profile.strip_prefix("sha256:") else {
        return false;
    };
    digest.len() == PROFILE_DIGEST_CHARACTERS
        && digest
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn size_and_rate_are_strict_positive_pairs() {
        assert_eq!(parse_size("640x480"), Ok((640, 480)));
        assert_eq!(parse_size("640X480"), Err(-libc::EINVAL));
        assert_eq!(parse_size("0x480"), Err(-libc::EINVAL));
        assert_eq!(parse_rate("1000/1"), Rate::new(1000, 1));
        assert_eq!(parse_rate("1000/0"), Err(-libc::EINVAL));
    }

    #[test]
    fn detector_profile_requires_exact_lowercase_sha256_syntax() {
        assert!(valid_profile(
            "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        ));
        assert!(!valid_profile(
            "sha256:0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"
        ));
        assert!(!valid_profile("sha256:abc"));
    }
}
