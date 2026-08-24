//! SPA ports and registered buffer ownership.

use std::ptr;

use libspa::sys;

use crate::Format;
use crate::format::FormatConstraint;

pub(crate) const MAX_BUFFERS: usize = 16;

/// Raw SPA port direction.
pub type Direction = sys::spa_direction;

/// Stable direction and numeric ID of one node port.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PortRef {
    /// SPA direction.
    pub direction: Direction,
    /// Direction-local numeric ID.
    pub id: u32,
}

pub(crate) struct BufferSlot {
    pub(crate) buffer: *mut sys::spa_buffer,
    pub(crate) available: bool,
}

impl BufferSlot {
    const fn empty() -> Self {
        Self {
            buffer: ptr::null_mut(),
            available: false,
        }
    }
}

/// One fixed SPA port owned by a node.
pub struct Port {
    pub(crate) key: PortRef,
    pub(crate) info: sys::spa_port_info,
    pub(crate) params: [sys::spa_param_info; 5],
    pub(crate) constraints: Box<[FormatConstraint]>,
    pub(crate) format: Option<Format>,
    pub(crate) buffers: [BufferSlot; MAX_BUFFERS],
    pub(crate) n_buffers: usize,
    pub(crate) io: *mut sys::spa_io_buffers,
    pub(crate) required: bool,
    pub(crate) configuration: bool,
}

// Ports are protected by the owning node callback gate. Registered SPA
// pointers are only dereferenced while the host owns the corresponding buffer.
unsafe impl Send for Port {}

impl Port {
    /// Creates a fixed port with one or more format alternatives.
    pub fn new(
        key: PortRef,
        required: bool,
        configuration: bool,
        constraints: impl Into<Box<[FormatConstraint]>>,
    ) -> Self {
        let mut flags = sys::SPA_PORT_FLAG_NO_REF as u64;
        if !required {
            flags |= sys::SPA_PORT_FLAG_OPTIONAL as u64;
        }
        let mut port = Self {
            key,
            info: sys::spa_port_info {
                change_mask: 0,
                flags,
                rate: sys::spa_fraction { num: 0, denom: 1 },
                props: ptr::null(),
                params: ptr::null_mut(),
                n_params: 0,
            },
            params: [param_info(0, 0); 5],
            constraints: constraints.into(),
            format: None,
            buffers: std::array::from_fn(|_| BufferSlot::empty()),
            n_buffers: 0,
            io: ptr::null_mut(),
            required,
            configuration,
        };
        port.params[0] = param_info(sys::SPA_PARAM_EnumFormat, sys::SPA_PARAM_INFO_READ);
        port.params[1] = param_info(sys::SPA_PARAM_IO, sys::SPA_PARAM_INFO_READ);
        port.params[4] = param_info(sys::SPA_PARAM_Meta, sys::SPA_PARAM_INFO_READ);
        port.update_format_params();
        port.info.params = port.params.as_mut_ptr();
        port.info.n_params = port.params.len() as u32;
        port
    }

    /// Returns the stable port reference.
    pub const fn key(&self) -> PortRef {
        self.key
    }

    /// Returns the currently negotiated fixed format.
    pub fn format(&self) -> Option<&Format> {
        self.format.as_ref()
    }

    /// Returns whether this is an optional prepared-object ingress.
    pub const fn is_configuration(&self) -> bool {
        self.configuration
    }

    /// Returns the input buffer currently offered by the host.
    pub fn input_buffer(&mut self) -> Result<Option<(u32, *mut sys::spa_buffer)>, i32> {
        if self.key.direction != sys::SPA_DIRECTION_INPUT {
            return Err(-libc::EINVAL);
        }
        let io = unsafe { self.io.as_mut() }.ok_or(-libc::EIO)?;
        if io.status != sys::SPA_STATUS_HAVE_DATA as i32 {
            return Ok(None);
        }
        let id = io.buffer_id;
        let slot = self
            .buffers
            .get(id as usize)
            .filter(|_| (id as usize) < self.n_buffers)
            .ok_or(-libc::EINVAL)?;
        Ok(Some((id, slot.buffer)))
    }

    /// Releases the current input buffer back to its upstream owner.
    pub fn consume_input(&mut self) -> Result<(), i32> {
        let io = unsafe { self.io.as_mut() }.ok_or(-libc::EIO)?;
        io.status = sys::SPA_STATUS_NEED_DATA as i32;
        Ok(())
    }

    /// Marks the current input buffer with a negative SPA error status.
    pub fn reject_input(&mut self, error: i32) -> Result<(), i32> {
        if error >= 0 {
            return Err(-libc::EINVAL);
        }
        let io = unsafe { self.io.as_mut() }.ok_or(-libc::EIO)?;
        io.status = error;
        Ok(())
    }

    /// Returns whether a previously published output is still held downstream.
    ///
    /// A returned buffer ID is reclaimed before the state is tested.
    pub fn output_pending(&mut self) -> Result<bool, i32> {
        if self.key.direction != sys::SPA_DIRECTION_OUTPUT {
            return Err(-libc::EINVAL);
        }
        let io = unsafe { self.io.as_mut() }.ok_or(-libc::EIO)?;
        if io.status == sys::SPA_STATUS_HAVE_DATA as i32 {
            return Ok(true);
        }
        if (io.buffer_id as usize) < self.n_buffers {
            self.buffers[io.buffer_id as usize].available = true;
            io.buffer_id = sys::SPA_ID_INVALID;
        }
        Ok(false)
    }

    /// Reserves one output buffer without publishing it.
    pub fn reserve_output(&mut self) -> Result<Option<(u32, *mut sys::spa_buffer)>, i32> {
        if self.output_pending()? {
            return Ok(None);
        }
        let Some(id) = self.buffers[..self.n_buffers]
            .iter()
            .position(|slot| slot.available)
        else {
            return Err(-libc::EPIPE);
        };
        self.buffers[id].available = false;
        Ok(Some((id as u32, self.buffers[id].buffer)))
    }

    /// Returns a reserved output buffer to this port without publishing it.
    pub fn cancel_output(&mut self, id: u32) -> Result<(), i32> {
        let slot = self
            .buffers
            .get_mut(id as usize)
            .filter(|_| (id as usize) < self.n_buffers)
            .ok_or(-libc::EINVAL)?;
        slot.available = true;
        Ok(())
    }

    /// Publishes a previously reserved output buffer.
    pub fn publish_output(&mut self, id: u32) -> Result<(), i32> {
        if id as usize >= self.n_buffers {
            return Err(-libc::EINVAL);
        }
        let io = unsafe { self.io.as_mut() }.ok_or(-libc::EIO)?;
        io.buffer_id = id;
        io.status = sys::SPA_STATUS_HAVE_DATA as i32;
        Ok(())
    }

    pub(crate) fn update_format_params(&mut self) {
        if self.format.is_some() {
            self.params[2] = param_info(sys::SPA_PARAM_Format, sys::SPA_PARAM_INFO_READWRITE);
            self.params[3] = param_info(sys::SPA_PARAM_Buffers, sys::SPA_PARAM_INFO_READ);
        } else {
            self.params[2] = param_info(sys::SPA_PARAM_Format, sys::SPA_PARAM_INFO_WRITE);
            self.params[3] = param_info(sys::SPA_PARAM_Buffers, 0);
        }
    }

    pub(crate) fn clear_buffers(&mut self) {
        self.buffers = std::array::from_fn(|_| BufferSlot::empty());
        self.n_buffers = 0;
    }

    pub(crate) fn ready(&self) -> bool {
        self.format.is_some() && !self.io.is_null() && self.n_buffers > 0
    }
}

pub(crate) const fn param_info(id: u32, flags: u32) -> sys::spa_param_info {
    sys::spa_param_info {
        id,
        flags,
        user: 0,
        seq: 0,
        padding: [0; 4],
    }
}
