//! Fixed SPA port formats and exact negotiation constraints.

use libspa::param::format::{ElementType, NdArrayLayout};
use libspa::sys;

/// Positive samples-per-interval rate carried by a negotiated stream format.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Rate {
    /// Samples-per-interval numerator.
    pub num: u32,
    /// Samples-per-interval denominator.
    pub denom: u32,
}

impl Rate {
    /// Constructs a positive rate.
    pub fn new(num: u32, denom: u32) -> Result<Self, i32> {
        if num == 0 || denom == 0 {
            return Err(-libc::EINVAL);
        }
        Ok(Self { num, denom })
    }
}

/// Logical family represented by one SPA format object.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FormatClass {
    /// Standard raw-video `GRAY8` detector carrier bytes.
    Gray8,
    /// Standard raw-video `GRAY16_LE` detector pixels.
    Gray16,
    /// PipeWireAO native `application/ndarray`.
    NdArray,
}

/// One completely fixed port format.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Format {
    /// Logical format family.
    pub class: FormatClass,
    /// Native ndarray element type, or `SPA_ELEMENT_TYPE_U16_LE` for GRAY16.
    pub element_type: u32,
    /// Logical shape in axis order. Images use `[height, width]`.
    pub shape: Box<[u32]>,
    /// Native ndarray layout, or row-major for GRAY16.
    pub layout: u32,
    /// Stream rate; absent only for prepared ndarray artifacts.
    pub rate: Option<Rate>,
    /// Scientific schema for ndarrays.
    pub schema: Option<Box<str>>,
    /// Exact interpretation profile for ndarrays.
    pub profile: Option<Box<str>>,
}

impl Format {
    /// Constructs an exact standard `GRAY8` detector format.
    pub fn gray8(width: u32, height: u32, rate: Rate) -> Result<Self, i32> {
        let format = Self {
            class: FormatClass::Gray8,
            element_type: sys::SPA_ELEMENT_TYPE_U8,
            shape: Box::new([height, width]),
            layout: sys::SPA_NDARRAY_LAYOUT_ROW_MAJOR,
            rate: Some(rate),
            schema: None,
            profile: None,
        };
        format.validate()?;
        Ok(format)
    }

    /// Constructs an exact standard `GRAY16_LE` detector format.
    pub fn gray16(width: u32, height: u32, rate: Rate) -> Result<Self, i32> {
        let format = Self {
            class: FormatClass::Gray16,
            element_type: sys::SPA_ELEMENT_TYPE_U16_LE,
            shape: Box::new([height, width]),
            layout: sys::SPA_NDARRAY_LAYOUT_ROW_MAJOR,
            rate: Some(rate),
            schema: None,
            profile: None,
        };
        format.validate()?;
        Ok(format)
    }

    /// Constructs an exact row-major F32 scientific image ndarray.
    pub fn f32_image(
        schema: impl Into<Box<str>>,
        profile: impl Into<Box<str>>,
        width: u32,
        height: u32,
        rate: Option<Rate>,
    ) -> Result<Self, i32> {
        let format = Self {
            class: FormatClass::NdArray,
            element_type: sys::SPA_ELEMENT_TYPE_F32_LE,
            shape: Box::new([height, width]),
            layout: sys::SPA_NDARRAY_LAYOUT_ROW_MAJOR,
            rate,
            schema: Some(schema.into()),
            profile: Some(profile.into()),
        };
        format.validate()?;
        Ok(format)
    }

    /// Constructs an exact row-major scientific ndarray.
    pub fn ndarray(
        element_type: u32,
        schema: impl Into<Box<str>>,
        profile: impl Into<Box<str>>,
        shape: impl Into<Box<[u32]>>,
        rate: Option<Rate>,
    ) -> Result<Self, i32> {
        Self::ndarray_with_layout(
            element_type,
            schema,
            profile,
            shape,
            sys::SPA_NDARRAY_LAYOUT_ROW_MAJOR,
            rate,
        )
    }

    /// Constructs an exact scientific ndarray with an explicit storage order.
    pub fn ndarray_with_layout(
        element_type: u32,
        schema: impl Into<Box<str>>,
        profile: impl Into<Box<str>>,
        shape: impl Into<Box<[u32]>>,
        layout: u32,
        rate: Option<Rate>,
    ) -> Result<Self, i32> {
        Self::ndarray_format(
            element_type,
            Some(schema.into()),
            Some(profile.into()),
            shape,
            layout,
            rate,
        )
    }

    /// Constructs an exact ndarray with optional semantic identity fields.
    pub fn ndarray_format(
        element_type: u32,
        schema: Option<Box<str>>,
        profile: Option<Box<str>>,
        shape: impl Into<Box<[u32]>>,
        layout: u32,
        rate: Option<Rate>,
    ) -> Result<Self, i32> {
        let format = Self {
            class: FormatClass::NdArray,
            element_type,
            shape: shape.into(),
            layout,
            rate,
            schema,
            profile,
        };
        format.validate()?;
        Ok(format)
    }

    /// Returns the image width.
    pub fn width(&self) -> Result<u32, i32> {
        self.shape.get(1).copied().ok_or(-libc::EINVAL)
    }

    /// Returns the image height.
    pub fn height(&self) -> Result<u32, i32> {
        self.shape.first().copied().ok_or(-libc::EINVAL)
    }

    /// Returns the packed byte count of one complete sample.
    pub fn packed_bytes(&self) -> Result<usize, i32> {
        self.element_count()?
            .checked_mul(self.element_size()?)
            .ok_or(-libc::EOVERFLOW)
    }

    /// Returns the packed bytes in one physical storage line.
    pub fn packed_stride(&self) -> Result<usize, i32> {
        if self.shape.len() == 1 {
            return self.element_size();
        }
        let extent = if self.layout == sys::SPA_NDARRAY_LAYOUT_COLUMN_MAJOR {
            self.shape.first()
        } else {
            self.shape.last()
        };
        (extent.copied().ok_or(-libc::EINVAL)? as usize)
            .checked_mul(self.element_size()?)
            .ok_or(-libc::EOVERFLOW)
    }

    /// Returns the number of physical storage lines represented by the chunk stride.
    pub fn line_count(&self) -> Result<usize, i32> {
        if self.shape.len() == 1 {
            return Ok(self.shape[0] as usize);
        }
        let dimensions = if self.layout == sys::SPA_NDARRAY_LAYOUT_COLUMN_MAJOR {
            &self.shape[1..]
        } else {
            &self.shape[..self.shape.len().saturating_sub(1)]
        };
        dimensions.iter().try_fold(1usize, |count, &dimension| {
            count
                .checked_mul(dimension as usize)
                .ok_or(-libc::EOVERFLOW)
        })
    }

    /// Returns whether image shape and rate are identical.
    pub fn same_shape_and_rate(&self, other: &Self) -> bool {
        self.shape == other.shape && self.rate == other.rate
    }

    fn element_count(&self) -> Result<usize, i32> {
        self.shape.iter().try_fold(1usize, |count, &dimension| {
            count
                .checked_mul(dimension as usize)
                .ok_or(-libc::EOVERFLOW)
        })
    }

    /// Returns the fixed packed size of one ndarray element.
    pub fn element_size(&self) -> Result<usize, i32> {
        ElementType::from_raw(self.element_type)
            .size()
            .ok_or(-libc::EINVAL)
    }

    pub(crate) fn validate(&self) -> Result<(), i32> {
        if self.shape.is_empty() || self.shape.contains(&0) {
            return Err(-libc::EINVAL);
        }
        if self
            .rate
            .is_some_and(|rate| rate.num == 0 || rate.denom == 0)
        {
            return Err(-libc::EINVAL);
        }
        match self.class {
            FormatClass::Gray8 | FormatClass::Gray16 => {
                let expected_type = match self.class {
                    FormatClass::Gray8 => sys::SPA_ELEMENT_TYPE_U8,
                    FormatClass::Gray16 => sys::SPA_ELEMENT_TYPE_U16_LE,
                    FormatClass::NdArray => unreachable!(),
                };
                if self.shape.len() != 2
                    || self.element_type != expected_type
                    || self.layout != sys::SPA_NDARRAY_LAYOUT_ROW_MAJOR
                    || self.rate.is_none()
                    || self.schema.is_some()
                    || self.profile.is_some()
                {
                    return Err(-libc::EINVAL);
                }
            }
            FormatClass::NdArray => {
                if ElementType::from_raw(self.element_type).size().is_none()
                    || !matches!(
                        NdArrayLayout::from_raw(self.layout),
                        NdArrayLayout::RowMajor | NdArrayLayout::ColumnMajor
                    )
                    || self.schema.as_deref().is_some_and(str::is_empty)
                    || self.profile.as_deref().is_some_and(str::is_empty)
                {
                    return Err(-libc::EINVAL);
                }
            }
        }
        if self.packed_bytes()? > i32::MAX as usize {
            return Err(-libc::EOVERFLOW);
        }
        Ok(())
    }
}

/// One exact alternative advertised by a port's `EnumFormat` parameter.
#[derive(Clone, Debug)]
pub struct FormatConstraint {
    format: Format,
}

impl FormatConstraint {
    /// Constructs an exact-format constraint.
    pub const fn exact(format: Format) -> Self {
        Self { format }
    }

    /// Returns the advertised exact format.
    pub const fn format(&self) -> &Format {
        &self.format
    }

    pub(crate) fn accepts(&self, format: &Format) -> bool {
        self.format == *format
    }
}
