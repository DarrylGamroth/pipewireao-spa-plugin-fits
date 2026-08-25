//! Validated views over registered SPA memory buffers.

use std::mem::{align_of, size_of};

use libspa::buffer::meta::{MetaProgressive, ProgressiveFlags, ProgressiveState};
use libspa::sys;

use crate::Format;

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
    rows: usize,
    header: Option<Header>,
}

/// One acquire-consistent observation of a progressively filled input frame.
pub struct ProgressiveInputObservation<'a> {
    values: &'a [u16],
    stride: usize,
    complete_rows: usize,
    state: ProgressiveState,
    terminal_flags: Option<ProgressiveFlags>,
    header: Option<Header>,
}

impl ProgressiveInputObservation<'_> {
    /// Borrows only the acquire-published U16 prefix.
    pub const fn u16(&self) -> (&[u16], usize) {
        (self.values, self.stride)
    }

    /// Returns the number of fully committed logical rows.
    pub const fn complete_rows(&self) -> usize {
        self.complete_rows
    }

    /// Returns the producer lifecycle state in the same acquire snapshot.
    pub const fn state(&self) -> ProgressiveState {
        self.state
    }

    /// Returns terminal flags after an aborted observation.
    pub const fn terminal_flags(&self) -> Option<ProgressiveFlags> {
        self.terminal_flags
    }

    /// Returns the current copy of standard header metadata.
    pub const fn header(&self) -> Option<Header> {
        self.header
    }
}

/// Validated access to a progressively filled `GRAY16_LE` input buffer.
pub struct ProgressiveInputFrame<'a> {
    buffer: *const sys::spa_buffer,
    data: *const u8,
    payload_offset: usize,
    payload_size: usize,
    row_bytes: usize,
    stride: usize,
    rows: usize,
    meta: &'a MetaProgressive,
}

impl<'a> ProgressiveInputFrame<'a> {
    /// Validates immutable progressive metadata and the negotiated layout.
    ///
    /// # Safety
    ///
    /// `buffer` and its metadata must remain registered until the associated
    /// input lease is consumed.
    pub unsafe fn new(buffer: *const sys::spa_buffer, format: &Format) -> Result<Self, i32> {
        let buffer_ref = unsafe { buffer.as_ref() }.ok_or(-libc::EINVAL)?;
        if buffer_ref.n_datas != 1 || buffer_ref.datas.is_null() {
            return Err(-libc::EINVAL);
        }
        let data = unsafe { &*buffer_ref.datas };
        if data.type_ != sys::SPA_DATA_MemPtr || data.data.is_null() {
            return Err(-libc::EINVAL);
        }
        let meta = unsafe {
            sys::spa_buffer_find_meta_data(
                buffer,
                sys::SPA_META_Progressive,
                size_of::<sys::spa_meta_progressive>(),
            )
            .cast::<MetaProgressive>()
            .as_ref()
        }
        .ok_or(-libc::ENODATA)?;
        meta.validate().map_err(|_| -libc::EPROTO)?;
        let raw = meta.as_raw();
        if raw.data_index != 0 {
            return Err(-libc::EINVAL);
        }
        let row_bytes = format.packed_stride()?;
        let rows = format.stride_count()?;
        let chunk = unsafe { data.chunk.as_ref() }.ok_or(-libc::EINVAL)?;
        let stride = read_stride(chunk.stride, row_bytes)?;
        let required = frame_span(row_bytes, stride, rows)?;
        let payload_offset = raw.payload_offset as usize;
        let payload_size = raw.payload_size as usize;
        if payload_size < required
            || payload_offset
                .checked_add(payload_size)
                .is_none_or(|end| end > data.maxsize as usize)
            || !(raw.commit_granularity as usize).is_multiple_of(stride)
        {
            return Err(-libc::EINVAL);
        }
        Ok(Self {
            buffer,
            data: data.data.cast::<u8>(),
            payload_offset,
            payload_size,
            row_bytes,
            stride,
            rows,
            meta,
        })
    }

    /// Acquire-loads publication state before exposing the immutable prefix.
    pub fn observe(&self) -> Result<ProgressiveInputObservation<'_>, i32> {
        let observation = self.meta.observe_acquire().map_err(|_| -libc::EPROTO)?;
        let snapshot = observation.snapshot();
        let committed = snapshot.committed_bytes() as usize;
        if committed > self.payload_size {
            return Err(-libc::EPROTO);
        }
        let complete_rows = if committed < self.row_bytes {
            0
        } else {
            (1 + (committed - self.row_bytes) / self.stride).min(self.rows)
        };
        let visible_bytes = if complete_rows == 0 {
            0
        } else {
            frame_span(self.row_bytes, self.stride, complete_rows)?
        };
        let byte_ptr = unsafe { self.data.add(self.payload_offset) };
        if !(byte_ptr as usize).is_multiple_of(align_of::<u16>())
            || !self.stride.is_multiple_of(size_of::<u16>())
            || !visible_bytes.is_multiple_of(size_of::<u16>())
        {
            return Err(-libc::EINVAL);
        }
        let values = unsafe {
            std::slice::from_raw_parts(byte_ptr.cast::<u16>(), visible_bytes / size_of::<u16>())
        };
        let header = unsafe { header(self.buffer).copied().map(Header::from) };
        Ok(ProgressiveInputObservation {
            values,
            stride: self.stride / size_of::<u16>(),
            complete_rows,
            state: snapshot.state(),
            terminal_flags: observation.terminal_flags(),
            header,
        })
    }
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
        if data.type_ != sys::SPA_DATA_MemPtr || data.data.is_null() || data.maxsize == 0 {
            return Err(-libc::EINVAL);
        }
        let row_bytes = format.packed_stride()?;
        let rows = format.stride_count()?;
        let stride = read_stride(chunk.stride, row_bytes)?;
        let span = frame_span(row_bytes, stride, rows)?;
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
            rows,
            header,
        })
    }

    /// Returns the complete byte span, including inter-row padding.
    pub const fn bytes(&self) -> &[u8] {
        self.bytes
    }

    /// Returns the physical byte distance between logical rows.
    pub const fn stride(&self) -> usize {
        self.stride
    }

    /// Returns the logical row count.
    pub const fn rows(&self) -> usize {
        self.rows
    }

    /// Returns copied header metadata when the buffer carries it.
    pub const fn header(&self) -> Option<Header> {
        self.header
    }

    /// Borrows aligned `u16` storage, including row padding before the final row.
    pub fn u16(&self) -> Result<(&[u16], usize), i32> {
        self.cast_with_stride()
    }

    /// Borrows aligned `u32` storage, including row padding before the final row.
    pub fn u32(&self) -> Result<(&[u32], usize), i32> {
        self.cast_with_stride()
    }

    /// Borrows one-byte storage.
    pub fn u8(&self) -> (&[u8], usize) {
        (self.bytes, self.stride)
    }

    /// Borrows aligned `f32` storage, including row padding before the final row.
    pub fn f32(&self) -> Result<(&[f32], usize), i32> {
        self.cast_with_stride()
    }

    /// Borrows aligned `f64` storage, including row padding before the final row.
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
    rows: usize,
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
        if data.type_ != sys::SPA_DATA_MemPtr || data.data.is_null() {
            return Err(-libc::EINVAL);
        }
        let row_bytes = format.packed_stride()?;
        let rows = format.stride_count()?;
        let stride = write_stride(chunk.stride, row_bytes);
        let span = frame_span(row_bytes, stride, rows)?;
        if span > data.maxsize as usize || span > u32::MAX as usize {
            return Err(-libc::ENOSPC);
        }
        let bytes = unsafe { std::slice::from_raw_parts_mut(data.data.cast::<u8>(), span) };
        let header = unsafe { header_mut(buffer) };
        Ok(Self {
            bytes,
            stride,
            rows,
            chunk,
            header,
        })
    }

    /// Returns the complete mutable byte span, including inter-row padding.
    pub fn bytes_mut(&mut self) -> &mut [u8] {
        self.bytes
    }

    /// Returns the physical byte distance between logical rows.
    pub const fn stride(&self) -> usize {
        self.stride
    }

    /// Returns the logical row count.
    pub const fn rows(&self) -> usize {
        self.rows
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

fn frame_span(row_bytes: usize, stride: usize, rows: usize) -> Result<usize, i32> {
    if rows == 0 {
        return Err(-libc::EINVAL);
    }
    stride
        .checked_mul(rows - 1)
        .and_then(|prefix| prefix.checked_add(row_bytes))
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
