//! Validated views over registered SPA memory buffers.

use std::mem::{align_of, size_of};

use libspa::sys;

use crate::Format;

pub(crate) fn is_cpu_mapped(type_: u32) -> bool {
    matches!(type_, sys::SPA_DATA_MemPtr | sys::SPA_DATA_MemFd)
}

/// Copyable standard SPA header metadata.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Header {
    /// SPA header flags.
    pub flags: u32,
    /// Position within the current graph cycle.
    pub offset: u32,
    /// Presentation timestamp in nanoseconds.
    pub pts: i64,
    /// Decoding timestamp offset from `pts`.
    pub dts_offset: i64,
    /// Stream sequence number or prepared-object identity.
    pub seq: u64,
}

impl From<sys::spa_meta_header> for Header {
    fn from(header: sys::spa_meta_header) -> Self {
        Self {
            flags: header.flags,
            offset: header.offset,
            pts: header.pts,
            dts_offset: header.dts_offset,
            seq: header.seq,
        }
    }
}

impl From<Header> for sys::spa_meta_header {
    fn from(header: Header) -> Self {
        Self {
            flags: header.flags,
            offset: header.offset,
            pts: header.pts,
            dts_offset: header.dts_offset,
            seq: header.seq,
        }
    }
}

/// Read-only validated complete-sample storage.
pub struct InputFrame<'a> {
    bytes: &'a [u8],
    stride: usize,
    lines: usize,
    header: Option<Header>,
}

impl<'a> InputFrame<'a> {
    /// Validates one registered SPA input buffer against its negotiated format.
    ///
    /// # Safety
    ///
    /// `buffer` must remain registered and exclusively host-owned for the
    /// returned borrow.
    pub unsafe fn new(buffer: *const sys::spa_buffer, format: &Format) -> Result<Self, i32> {
        let buffer = unsafe { buffer.as_ref() }.ok_or(-libc::EINVAL)?;
        if buffer.n_datas != 1 || buffer.datas.is_null() {
            return Err(-libc::EINVAL);
        }
        let data = unsafe { &*buffer.datas };
        let chunk = unsafe { data.chunk.as_ref() }.ok_or(-libc::EINVAL)?;
        if !is_cpu_mapped(data.type_) || data.data.is_null() || data.maxsize == 0 {
            return Err(-libc::EINVAL);
        }
        let line_bytes = format.packed_stride()?;
        let lines = format.line_count()?;
        let stride = read_stride(chunk.stride, line_bytes)?;
        let span = frame_span(line_bytes, stride, lines)?;
        let offset = chunk.offset as usize % data.maxsize as usize;
        if (chunk.size as usize) < span
            || offset
                .checked_add(span)
                .is_none_or(|end| end > data.maxsize as usize)
        {
            return Err(-libc::EINVAL);
        }
        let bytes = unsafe { std::slice::from_raw_parts(data.data.cast::<u8>().add(offset), span) };
        let header = unsafe { header(buffer).copied().map(Header::from) };
        Ok(Self {
            bytes,
            stride,
            lines,
            header,
        })
    }

    /// Returns the complete byte span, including inter-line padding.
    pub const fn bytes(&self) -> &[u8] {
        self.bytes
    }

    /// Returns the physical byte distance between storage lines.
    pub const fn stride(&self) -> usize {
        self.stride
    }

    /// Returns the physical storage-line count.
    pub const fn lines(&self) -> usize {
        self.lines
    }

    /// Returns copied header metadata when the buffer carries it.
    pub const fn header(&self) -> Option<Header> {
        self.header
    }

    /// Borrows aligned `u16` storage, including line padding before the final line.
    pub fn u16(&self) -> Result<(&[u16], usize), i32> {
        self.cast_with_stride()
    }

    /// Borrows aligned `u32` storage, including line padding before the final line.
    pub fn u32(&self) -> Result<(&[u32], usize), i32> {
        self.cast_with_stride()
    }

    /// Borrows one-byte storage.
    pub fn u8(&self) -> (&[u8], usize) {
        (self.bytes, self.stride)
    }

    /// Borrows aligned `f32` storage, including line padding before the final line.
    pub fn f32(&self) -> Result<(&[f32], usize), i32> {
        self.cast_with_stride()
    }

    /// Borrows aligned `f64` storage, including line padding before the final line.
    pub fn f64(&self) -> Result<(&[f64], usize), i32> {
        self.cast_with_stride()
    }

    fn cast_with_stride<T>(&self) -> Result<(&[T], usize), i32> {
        if !(self.bytes.as_ptr() as usize).is_multiple_of(align_of::<T>())
            || !self.bytes.len().is_multiple_of(size_of::<T>())
            || !self.stride.is_multiple_of(size_of::<T>())
        {
            return Err(-libc::EINVAL);
        }
        let values = unsafe {
            std::slice::from_raw_parts(
                self.bytes.as_ptr().cast::<T>(),
                self.bytes.len() / size_of::<T>(),
            )
        };
        Ok((values, self.stride / size_of::<T>()))
    }
}

/// Writable validated complete-sample storage.
pub struct OutputFrame<'a> {
    bytes: &'a mut [u8],
    stride: usize,
    lines: usize,
    chunk: &'a mut sys::spa_chunk,
    header: Option<&'a mut sys::spa_meta_header>,
}

impl<'a> OutputFrame<'a> {
    /// Validates one registered SPA output buffer against its negotiated format.
    ///
    /// # Safety
    ///
    /// `buffer` must remain registered and exclusively owned by the node for
    /// the returned mutable borrow.
    pub unsafe fn new(buffer: *mut sys::spa_buffer, format: &Format) -> Result<Self, i32> {
        let buffer = unsafe { buffer.as_mut() }.ok_or(-libc::EINVAL)?;
        if buffer.n_datas != 1 || buffer.datas.is_null() {
            return Err(-libc::EINVAL);
        }
        let data = unsafe { &mut *buffer.datas };
        let chunk = unsafe { data.chunk.as_mut() }.ok_or(-libc::EINVAL)?;
        if !is_cpu_mapped(data.type_) || data.data.is_null() {
            return Err(-libc::EINVAL);
        }
        let line_bytes = format.packed_stride()?;
        let lines = format.line_count()?;
        let stride = write_stride(chunk.stride, line_bytes);
        let span = frame_span(line_bytes, stride, lines)?;
        if span > data.maxsize as usize || span > u32::MAX as usize {
            return Err(-libc::ENOSPC);
        }
        let bytes = unsafe { std::slice::from_raw_parts_mut(data.data.cast::<u8>(), span) };
        let header = unsafe { header_mut(buffer) };
        Ok(Self {
            bytes,
            stride,
            lines,
            chunk,
            header,
        })
    }

    /// Returns the complete mutable byte span, including inter-line padding.
    pub fn bytes_mut(&mut self) -> &mut [u8] {
        self.bytes
    }

    /// Returns the physical byte distance between storage lines.
    pub const fn stride(&self) -> usize {
        self.stride
    }

    /// Returns the physical storage-line count.
    pub const fn lines(&self) -> usize {
        self.lines
    }

    /// Copies input header metadata when this output has standard Header meta.
    pub fn set_header(&mut self, header: Option<Header>) {
        if let (Some(destination), Some(source)) = (self.header.as_deref_mut(), header) {
            *destination = source.into();
        }
    }

    /// Borrows aligned mutable `u8` storage.
    pub fn u8_mut(&mut self) -> (&mut [u8], usize) {
        (self.bytes, self.stride)
    }

    /// Borrows aligned mutable `u16` storage.
    pub fn u16_mut(&mut self) -> Result<(&mut [u16], usize), i32> {
        self.cast_with_stride()
    }

    /// Borrows aligned mutable `f32` storage.
    pub fn f32_mut(&mut self) -> Result<(&mut [f32], usize), i32> {
        self.cast_with_stride()
    }

    /// Borrows aligned mutable `f64` storage.
    pub fn f64_mut(&mut self) -> Result<(&mut [f64], usize), i32> {
        self.cast_with_stride()
    }

    /// Marks the complete output span valid for publication.
    pub fn commit(&mut self) {
        self.chunk.offset = 0;
        self.chunk.size = self.bytes.len() as u32;
        self.chunk.stride = self.stride as i32;
        self.chunk.flags = 0;
    }

    fn cast_with_stride<T>(&mut self) -> Result<(&mut [T], usize), i32> {
        if !(self.bytes.as_ptr() as usize).is_multiple_of(align_of::<T>())
            || !self.bytes.len().is_multiple_of(size_of::<T>())
            || !self.stride.is_multiple_of(size_of::<T>())
        {
            return Err(-libc::EINVAL);
        }
        let length = self.bytes.len() / size_of::<T>();
        let values =
            unsafe { std::slice::from_raw_parts_mut(self.bytes.as_mut_ptr().cast(), length) };
        Ok((values, self.stride / size_of::<T>()))
    }
}

unsafe fn header(buffer: *const sys::spa_buffer) -> Option<&'static sys::spa_meta_header> {
    unsafe {
        sys::spa_buffer_find_meta_data(
            buffer,
            sys::SPA_META_Header,
            size_of::<sys::spa_meta_header>(),
        )
        .cast::<sys::spa_meta_header>()
        .as_ref()
    }
}

unsafe fn header_mut(buffer: *mut sys::spa_buffer) -> Option<&'static mut sys::spa_meta_header> {
    unsafe {
        sys::spa_buffer_find_meta_data(
            buffer,
            sys::SPA_META_Header,
            size_of::<sys::spa_meta_header>(),
        )
        .cast::<sys::spa_meta_header>()
        .as_mut()
    }
}

fn read_stride(stride: i32, packed: usize) -> Result<usize, i32> {
    if stride == 0 {
        Ok(packed)
    } else {
        let stride = usize::try_from(stride).map_err(|_| -libc::EINVAL)?;
        (stride >= packed).then_some(stride).ok_or(-libc::EINVAL)
    }
}

fn write_stride(stride: i32, packed: usize) -> usize {
    if stride > 0 && stride as usize >= packed {
        stride as usize
    } else {
        packed
    }
}

fn frame_span(line_bytes: usize, stride: usize, lines: usize) -> Result<usize, i32> {
    if lines == 0 {
        return Err(-libc::EINVAL);
    }
    stride
        .checked_mul(lines - 1)
        .and_then(|prefix| prefix.checked_add(line_bytes))
        .ok_or(-libc::EOVERFLOW)
}

/// Returns whether two validated frames overlap in memory.
pub fn overlaps(input: &InputFrame<'_>, output: &OutputFrame<'_>) -> bool {
    let input_start = input.bytes.as_ptr() as usize;
    let input_end = input_start.saturating_add(input.bytes.len());
    let output_start = output.bytes.as_ptr() as usize;
    let output_end = output_start.saturating_add(output.bytes.len());
    input_start < output_end && output_start < input_end
}

/// Returns whether two registered buffers refer to the same first data block.
///
/// # Safety
///
/// Both buffer pointers must remain registered for the duration of this call.
pub unsafe fn shares_data(input: *const sys::spa_buffer, output: *const sys::spa_buffer) -> bool {
    let Some(input) = (unsafe { input.as_ref() }) else {
        return false;
    };
    let Some(output) = (unsafe { output.as_ref() }) else {
        return false;
    };
    if input.n_datas != 1 || input.datas.is_null() || output.n_datas != 1 || output.datas.is_null()
    {
        return false;
    }
    let input_data = unsafe { &*input.datas };
    let output_data = unsafe { &*output.datas };
    !input_data.data.is_null() && input_data.data == output_data.data
}

/// Forwards a complete frame when the input and output buffers share data storage.
///
/// The payload is not copied. Chunk state and standard Header metadata are made
/// visible through the output buffer.
///
/// # Safety
///
/// Both buffers must remain registered and host-owned for the duration of this
/// call. Their shared storage must have been established by SPA buffer
/// allocation negotiation.
pub unsafe fn forward_frame(
    input: *const sys::spa_buffer,
    output: *mut sys::spa_buffer,
    format: &Format,
    frame_header: Option<Header>,
) -> Result<bool, i32> {
    unsafe { InputFrame::new(input, format)? };

    let input = unsafe { input.as_ref() }.ok_or(-libc::EINVAL)?;
    let output = unsafe { output.as_mut() }.ok_or(-libc::EINVAL)?;
    if input.n_datas != 1 || input.datas.is_null() || output.n_datas != 1 || output.datas.is_null()
    {
        return Err(-libc::EINVAL);
    }
    let input_data = unsafe { &*input.datas };
    let output_data = unsafe { &mut *output.datas };
    if !is_cpu_mapped(input_data.type_)
        || !is_cpu_mapped(output_data.type_)
        || input_data.data.is_null()
        || output_data.data.is_null()
        || input_data.data != output_data.data
    {
        return Ok(false);
    }

    let input_chunk = unsafe { input_data.chunk.as_ref() }.ok_or(-libc::EINVAL)?;
    let chunk = *input_chunk;
    let output_chunk = unsafe { output_data.chunk.as_mut() }.ok_or(-libc::EINVAL)?;
    if !std::ptr::eq(input_chunk, output_chunk) {
        *output_chunk = chunk;
    }
    if let (Some(destination), Some(source)) = (unsafe { header_mut(output) }, frame_header) {
        *destination = source.into();
    }
    Ok(true)
}
