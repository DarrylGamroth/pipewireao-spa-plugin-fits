//! Construction and parsing of SPA POD values.

use std::io::Cursor;
use std::mem::size_of;
use std::ptr;

use libspa::param::format::{ElementType, MatrixFormat, NdArrayLayout};
use libspa::pod::deserialize::PodDeserializer;
use libspa::pod::serialize::PodSerializer;
use libspa::pod::{ChoiceValue, Object, Value};
use libspa::sys;
use libspa::utils::{Choice, ChoiceEnum, ChoiceFlags, Fraction, Id, Rectangle};

use crate::format::{Format, FormatClass, FormatConstraint, Rate};

/// One property within an SPA object POD.
pub use libspa::pod::Property;

fn property(key: u32, value: Value) -> Property {
    Property::new(key, value)
}

/// Constructs an SPA object value.
pub fn object(type_: u32, id: u32, properties: Vec<Property>) -> Value {
    Value::Object(Object {
        type_,
        id,
        properties,
    })
}

fn id(value: u32) -> Value {
    Value::Id(Id(value))
}

fn int_range(default: i32, min: i32, max: i32) -> Value {
    Value::Choice(ChoiceValue::Int(Choice(
        ChoiceFlags::empty(),
        ChoiceEnum::Range { default, min, max },
    )))
}

pub(crate) fn port_param(
    id_: u32,
    index: u32,
    constraints: &[FormatConstraint],
    format: Option<&Format>,
) -> Result<Option<Value>, i32> {
    let value = match id_ {
        sys::SPA_PARAM_EnumFormat => {
            let Some(constraint) = constraints.get(index as usize) else {
                return Ok(None);
            };
            format_value(constraint.format(), sys::SPA_PARAM_EnumFormat)
        }
        sys::SPA_PARAM_Format if index == 0 => {
            format_value(format.ok_or(-libc::EIO)?, sys::SPA_PARAM_Format)
        }
        sys::SPA_PARAM_Buffers if index == 0 => buffer_param(format.ok_or(-libc::EIO)?)?,
        sys::SPA_PARAM_Meta if index == 0 => object(
            sys::SPA_TYPE_OBJECT_ParamMeta,
            id_,
            vec![
                property(sys::SPA_PARAM_META_type, id(sys::SPA_META_Header)),
                property(
                    sys::SPA_PARAM_META_size,
                    Value::Int(size_of::<sys::spa_meta_header>() as i32),
                ),
            ],
        ),
        sys::SPA_PARAM_IO if index == 0 => object(
            sys::SPA_TYPE_OBJECT_ParamIO,
            id_,
            vec![
                property(sys::SPA_PARAM_IO_id, id(sys::SPA_IO_Buffers)),
                property(
                    sys::SPA_PARAM_IO_size,
                    Value::Int(size_of::<sys::spa_io_buffers>() as i32),
                ),
            ],
        ),
        sys::SPA_PARAM_Format
        | sys::SPA_PARAM_Buffers
        | sys::SPA_PARAM_Meta
        | sys::SPA_PARAM_IO => return Ok(None),
        _ => return Err(-libc::ENOENT),
    };
    Ok(Some(value))
}

fn format_value(format: &Format, object_id: u32) -> Value {
    match format.class {
        FormatClass::Gray16 => object(
            sys::SPA_TYPE_OBJECT_Format,
            object_id,
            vec![
                property(sys::SPA_FORMAT_mediaType, id(sys::SPA_MEDIA_TYPE_video)),
                property(sys::SPA_FORMAT_mediaSubtype, id(sys::SPA_MEDIA_SUBTYPE_raw)),
                property(
                    sys::SPA_FORMAT_VIDEO_format,
                    id(sys::SPA_VIDEO_FORMAT_GRAY16_LE),
                ),
                property(
                    sys::SPA_FORMAT_VIDEO_size,
                    Value::Rectangle(Rectangle {
                        width: format.width().expect("validated width"),
                        height: format.height().expect("validated height"),
                    }),
                ),
                property(
                    sys::SPA_FORMAT_VIDEO_framerate,
                    Value::Fraction(rate_fraction(format.rate.expect("validated video rate"))),
                ),
            ],
        ),
        FormatClass::NdArray => {
            let mut properties = MatrixFormat::new(
                ElementType::F32Le,
                format.height().expect("validated height"),
                format.width().expect("validated width"),
                NdArrayLayout::RowMajor,
                format.rate.map(rate_fraction),
            )
            .expect("validated ndarray format")
            .properties();
            properties.push(property(
                sys::SPA_FORMAT_NDARRAY_schema,
                Value::String(format.schema.as_deref().expect("validated schema").into()),
            ));
            properties.push(property(
                sys::SPA_FORMAT_NDARRAY_profile,
                Value::String(format.profile.as_deref().expect("validated profile").into()),
            ));
            object(sys::SPA_TYPE_OBJECT_Format, object_id, properties)
        }
    }
}

fn rate_fraction(rate: Rate) -> Fraction {
    Fraction {
        num: rate.num,
        denom: rate.denom,
    }
}

fn buffer_param(format: &Format) -> Result<Value, i32> {
    let stride = i32::try_from(format.packed_stride()?).map_err(|_| -libc::EOVERFLOW)?;
    let size = i32::try_from(format.packed_bytes()?).map_err(|_| -libc::EOVERFLOW)?;
    let memory = 1_i32
        .checked_shl(sys::SPA_DATA_MemPtr)
        .ok_or(-libc::EOVERFLOW)?;
    Ok(object(
        sys::SPA_TYPE_OBJECT_ParamBuffers,
        sys::SPA_PARAM_Buffers,
        vec![
            property(sys::SPA_PARAM_BUFFERS_buffers, int_range(3, 2, 16)),
            property(sys::SPA_PARAM_BUFFERS_blocks, Value::Int(1)),
            property(sys::SPA_PARAM_BUFFERS_size, Value::Int(size)),
            property(sys::SPA_PARAM_BUFFERS_stride, Value::Int(stride)),
            property(
                sys::SPA_PARAM_BUFFERS_dataType,
                Value::Choice(ChoiceValue::Int(Choice(
                    ChoiceFlags::empty(),
                    ChoiceEnum::Flags {
                        default: memory,
                        flags: vec![memory],
                    },
                ))),
            ),
        ],
    ))
}

fn unique(properties: &[Property], key: u32) -> Result<Option<&Value>, i32> {
    let mut values = properties
        .iter()
        .filter(|property| property.key == key)
        .map(|property| &property.value);
    let value = values.next();
    if values.next().is_some() {
        return Err(-libc::EINVAL);
    }
    Ok(value)
}

pub(crate) fn parse_format(value: Value, constraints: &[FormatConstraint]) -> Result<Format, i32> {
    let Value::Object(object) = value else {
        return Err(-libc::EPROTOTYPE);
    };
    if object.type_ != sys::SPA_TYPE_OBJECT_Format {
        return Err(-libc::EPROTOTYPE);
    }
    let media_type = match unique(&object.properties, sys::SPA_FORMAT_mediaType)? {
        Some(Value::Id(Id(value))) => *value,
        _ => return Err(-libc::EINVAL),
    };
    let media_subtype = match unique(&object.properties, sys::SPA_FORMAT_mediaSubtype)? {
        Some(Value::Id(Id(value))) => *value,
        _ => return Err(-libc::EINVAL),
    };

    let format =
        if media_type == sys::SPA_MEDIA_TYPE_video && media_subtype == sys::SPA_MEDIA_SUBTYPE_raw {
            parse_gray16(&object.properties)?
        } else if media_type == sys::SPA_MEDIA_TYPE_application
            && media_subtype == sys::SPA_MEDIA_SUBTYPE_ndarray
        {
            parse_ndarray(&object.properties)?
        } else {
            return Err(-libc::EINVAL);
        };
    format.validate()?;
    if !constraints
        .iter()
        .any(|constraint| constraint.accepts(&format))
    {
        return Err(-libc::EINVAL);
    }
    Ok(format)
}

fn parse_gray16(properties: &[Property]) -> Result<Format, i32> {
    match unique(properties, sys::SPA_FORMAT_VIDEO_format)? {
        Some(Value::Id(Id(value))) if *value == sys::SPA_VIDEO_FORMAT_GRAY16_LE => {}
        _ => return Err(-libc::EINVAL),
    }
    let size = match unique(properties, sys::SPA_FORMAT_VIDEO_size)? {
        Some(Value::Rectangle(size)) => *size,
        _ => return Err(-libc::EINVAL),
    };
    let rate = match unique(properties, sys::SPA_FORMAT_VIDEO_framerate)? {
        Some(Value::Fraction(rate)) => Rate::new(rate.num, rate.denom)?,
        _ => return Err(-libc::EINVAL),
    };
    Format::gray16(size.width, size.height, rate)
}

fn parse_ndarray(properties: &[Property]) -> Result<Format, i32> {
    let native = MatrixFormat::from_properties(properties).map_err(|_| -libc::EINVAL)?;
    let schema = match unique(properties, sys::SPA_FORMAT_NDARRAY_schema)? {
        Some(Value::String(schema)) if !schema.is_empty() => schema.clone().into_boxed_str(),
        _ => return Err(-libc::EINVAL),
    };
    let profile = match unique(properties, sys::SPA_FORMAT_NDARRAY_profile)? {
        Some(Value::String(profile)) if !profile.is_empty() => profile.clone().into_boxed_str(),
        _ => return Err(-libc::EINVAL),
    };
    let native = native.as_ndarray();
    let [height, width] = native.shape() else {
        return Err(-libc::EINVAL);
    };
    if native.element_type() != ElementType::F32Le || native.layout() != NdArrayLayout::RowMajor {
        return Err(-libc::EINVAL);
    }
    Format::f32_image(
        schema,
        profile,
        *width,
        *height,
        native.rate().map(|rate| Rate {
            num: rate.num,
            denom: rate.denom,
        }),
    )
}

/// Returns the properties from a correctly typed Props object.
pub fn parse_props(value: Value) -> Result<Vec<Property>, i32> {
    let Value::Object(object) = value else {
        return Err(-libc::EPROTOTYPE);
    };
    if object.type_ != sys::SPA_TYPE_OBJECT_Props || object.id != sys::SPA_PARAM_Props {
        return Err(-libc::EPROTOTYPE);
    }
    Ok(object.properties)
}

pub(crate) unsafe fn decode(pod: *const sys::spa_pod) -> Result<Value, i32> {
    let pod = unsafe { pod.as_ref() }.ok_or(-libc::EINVAL)?;
    let length = size_of::<sys::spa_pod>()
        .checked_add(pod.size as usize)
        .ok_or(-libc::EOVERFLOW)?;
    let bytes = unsafe { std::slice::from_raw_parts(ptr::from_ref(pod).cast::<u8>(), length) };
    let (remaining, value) =
        PodDeserializer::deserialize_any_from(bytes).map_err(|_| -libc::EINVAL)?;
    if !remaining.is_empty() {
        return Err(-libc::EINVAL);
    }
    Ok(value)
}

pub(crate) unsafe fn with_filtered_pod<R>(
    value: &Value,
    filter: *const sys::spa_pod,
    operation: impl FnOnce(*mut sys::spa_pod) -> R,
) -> Result<Option<R>, i32> {
    let (cursor, _) = PodSerializer::serialize(Cursor::new(Vec::with_capacity(1024)), value)
        .map_err(|_| -libc::EINVAL)?;
    let encoded = cursor.into_inner();
    let mut source_words = vec![0_u64; encoded.len().div_ceil(size_of::<u64>())];
    let source_bytes = unsafe {
        std::slice::from_raw_parts_mut(
            source_words.as_mut_ptr().cast::<u8>(),
            source_words.len() * size_of::<u64>(),
        )
    };
    source_bytes[..encoded.len()].copy_from_slice(&encoded);
    let source = source_words.as_mut_ptr().cast::<sys::spa_pod>();
    if filter.is_null() {
        return Ok(Some(operation(source)));
    }

    let filtered_bytes = encoded.len().saturating_mul(2).max(4096);
    let mut filtered_words = vec![0_u64; filtered_bytes.div_ceil(size_of::<u64>())];
    let mut builder = std::mem::MaybeUninit::<sys::spa_pod_builder>::uninit();
    unsafe {
        sys::spa_pod_builder_init(
            builder.as_mut_ptr(),
            filtered_words.as_mut_ptr().cast(),
            u32::try_from(filtered_words.len() * size_of::<u64>()).map_err(|_| -libc::EOVERFLOW)?,
        );
    }
    let mut builder = unsafe { builder.assume_init() };
    let mut result = ptr::null_mut();
    let status = unsafe { sys::spa_pod_filter(&mut builder, &mut result, source, filter) };
    if status < 0 {
        return Ok(None);
    }
    if result.is_null() {
        return Err(-libc::EIO);
    }
    Ok(Some(operation(result)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use libspa::pod::ValueArray;

    const PROFILE: &str = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

    #[test]
    fn fixed_native_ndarray_round_trips() {
        let format = Format::f32_image("org.calculon.test/1", PROFILE, 4, 3, None).unwrap();
        let constraint = FormatConstraint::exact(format.clone());
        let value = format_value(&format, sys::SPA_PARAM_Format);
        let parsed = unsafe {
            with_filtered_pod(&value, ptr::null(), |pod| {
                parse_format(decode(pod).unwrap(), &[constraint]).unwrap()
            })
            .unwrap()
            .unwrap()
        };
        assert_eq!(parsed, format);
    }

    #[test]
    fn missing_schema_is_rejected() {
        let format = Format::f32_image("org.calculon.test/1", PROFILE, 4, 3, None).unwrap();
        let constraint = FormatConstraint::exact(format.clone());
        let Value::Object(mut object) = format_value(&format, sys::SPA_PARAM_Format) else {
            unreachable!();
        };
        object
            .properties
            .retain(|property| property.key != sys::SPA_FORMAT_NDARRAY_schema);
        assert_eq!(
            parse_format(Value::Object(object), &[constraint]),
            Err(-libc::EINVAL)
        );
    }

    #[test]
    fn wrong_profile_is_rejected_by_exact_constraint() {
        let expected = Format::f32_image("org.calculon.test/1", PROFILE, 4, 3, None).unwrap();
        let candidate = Format::f32_image(
            "org.calculon.test/1",
            "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            4,
            3,
            None,
        )
        .unwrap();
        let value = format_value(&candidate, sys::SPA_PARAM_Format);
        assert_eq!(
            parse_format(value, &[FormatConstraint::exact(expected)]),
            Err(-libc::EINVAL)
        );
    }

    #[test]
    fn ndarray_shape_is_height_then_width() {
        let format = Format::f32_image("org.calculon.test/1", PROFILE, 4, 3, None).unwrap();
        let Value::Object(object) = format_value(&format, sys::SPA_PARAM_Format) else {
            unreachable!();
        };
        assert_eq!(
            unique(&object.properties, sys::SPA_FORMAT_NDARRAY_shape).unwrap(),
            Some(&Value::ValueArray(ValueArray::Int(vec![3, 4])))
        );
    }
}
