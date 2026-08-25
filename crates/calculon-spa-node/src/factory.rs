//! Generic Rust implementation of SPA factory and node callbacks.

use std::cell::UnsafeCell;
use std::ffi::{CStr, c_char, c_void};
use std::marker::PhantomData;
use std::mem::{ManuallyDrop, size_of};
use std::ops::{Deref, DerefMut};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::sync::atomic::{AtomicBool, Ordering};

use libspa::pod::Value;
use libspa::sys;

use crate::Format;
use crate::pod;
use crate::port::{MAX_BUFFERS, Port, PortRef, param_info};

/// Behavior supplied by one concrete SPA node factory.
pub trait Node: Send + Sized + 'static {
    /// Whether the node exposes `PropInfo` and `Props` parameters.
    const HAS_PROPS: bool = false;

    /// Constructs one factory instance.
    fn new(info: Option<&sys::spa_dict>) -> Result<Self, i32>;

    /// Returns fixed ports in stable storage.
    fn ports(&self) -> &[Port];

    /// Returns fixed ports in stable storage mutably.
    fn ports_mut(&mut self) -> &mut [Port];

    /// Enumerates one node-level parameter value.
    fn enum_param(&self, _id: u32, _index: u32) -> Result<Option<Value>, i32> {
        Err(-libc::ENOENT)
    }

    /// Applies or tests one node-level parameter.
    fn set_param(
        &mut self,
        _id: u32,
        _flags: u32,
        _value: Option<Value>,
        _started: bool,
    ) -> Result<(), i32> {
        Err(-libc::ENOENT)
    }

    /// Validates a proposed fixed format before it is installed.
    fn validate_format(&self, _port: usize, _format: Option<&Format>) -> Result<(), i32> {
        Ok(())
    }

    /// Updates prepared storage after one port format changes.
    fn format_changed(&mut self, _port: usize) -> Result<(), i32> {
        Ok(())
    }

    /// Validates node-specific readiness before `Start` succeeds.
    fn ready(&self) -> Result<(), i32> {
        Ok(())
    }

    /// Performs node-specific start preparation.
    fn start(&mut self) -> Result<(), i32> {
        Ok(())
    }

    /// Performs node-specific pause handling.
    fn pause(&mut self) {}

    /// Runs one complete-frame processing step.
    fn process(&mut self) -> Result<i32, i32>;
}

struct State<N> {
    node: N,
    started: bool,
    params: [sys::spa_param_info; 2],
}

/// Serializes callbacks without making a real-time caller wait.
struct CallbackGate<T> {
    claimed: AtomicBool,
    value: UnsafeCell<T>,
}

impl<T> CallbackGate<T> {
    fn new(value: T) -> Self {
        Self {
            claimed: AtomicBool::new(false),
            value: UnsafeCell::new(value),
        }
    }

    fn claim(&self) -> Result<CallbackGuard<'_, T>, i32> {
        self.claimed
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .map_err(|_| -libc::EBUSY)?;
        Ok(CallbackGuard { gate: self })
    }
}

// The atomic claim gives one callback exclusive access to the contained value.
unsafe impl<T: Send> Sync for CallbackGate<T> {}

struct CallbackGuard<'a, T> {
    gate: &'a CallbackGate<T>,
}

impl<T> Deref for CallbackGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        unsafe { &*self.gate.value.get() }
    }
}

impl<T> DerefMut for CallbackGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        unsafe { &mut *self.gate.value.get() }
    }
}

impl<T> Drop for CallbackGuard<'_, T> {
    fn drop(&mut self) {
        self.gate.claimed.store(false, Ordering::Release);
    }
}

impl<N: Node> State<N> {
    fn new(node: N) -> Self {
        Self {
            node,
            started: false,
            params: [
                param_info(sys::SPA_PARAM_PropInfo, sys::SPA_PARAM_INFO_READ),
                param_info(sys::SPA_PARAM_Props, sys::SPA_PARAM_INFO_READWRITE),
            ],
        }
    }

    fn node_info(&mut self) -> sys::spa_node_info {
        let input_count = self
            .node
            .ports()
            .iter()
            .filter(|port| port.key.direction == sys::SPA_DIRECTION_INPUT)
            .count();
        let output_count = self.node.ports().len() - input_count;
        sys::spa_node_info {
            max_input_ports: input_count as u32,
            max_output_ports: output_count as u32,
            change_mask: 0,
            flags: self.node_flags(),
            props: ptr::null_mut(),
            params: if N::HAS_PROPS {
                self.params.as_mut_ptr()
            } else {
                ptr::null_mut()
            },
            n_params: if N::HAS_PROPS {
                self.params.len() as u32
            } else {
                0
            },
        }
    }

    fn node_flags(&self) -> u64 {
        let mut flags = sys::SPA_NODE_FLAG_RT as u64;
        if self
            .node
            .ports()
            .iter()
            .any(|port| port.required && port.format.is_none())
        {
            flags |= sys::SPA_NODE_FLAG_NEED_CONFIGURE as u64;
        }
        flags
    }

    fn ready(&self) -> Result<(), i32> {
        if self
            .node
            .ports()
            .iter()
            .any(|port| (port.required || port.format.is_some()) && !port.ready())
        {
            return Err(-libc::EIO);
        }
        self.node.ready()
    }
}

#[repr(C)]
struct Handle<N: Node> {
    handle: sys::spa_handle,
    node: sys::spa_node,
    hooks: sys::spa_hook_list,
    state: ManuallyDrop<CallbackGate<State<N>>>,
}

#[repr(transparent)]
struct SyncValue<T>(T);

// Factory and method tables are immutable for the process lifetime.
unsafe impl<T> Sync for SyncValue<T> {}

struct Methods<N>(PhantomData<N>);

impl<N: Node> Methods<N> {
    const VALUE: sys::spa_node_methods = sys::spa_node_methods {
        version: sys::SPA_VERSION_NODE_METHODS,
        add_listener: Some(node_add_listener::<N>),
        set_callbacks: Some(node_set_callbacks::<N>),
        sync: None,
        enum_params: Some(node_enum_params::<N>),
        set_param: Some(node_set_param::<N>),
        set_io: Some(node_set_io::<N>),
        send_command: Some(node_send_command::<N>),
        add_port: Some(node_add_port::<N>),
        remove_port: Some(node_remove_port::<N>),
        port_enum_params: Some(node_port_enum_params::<N>),
        port_set_param: Some(node_port_set_param::<N>),
        port_use_buffers: Some(node_port_use_buffers::<N>),
        port_set_io: Some(node_port_set_io::<N>),
        port_reuse_buffer: Some(node_port_reuse_buffer::<N>),
        process: Some(node_process::<N>),
    };
}

static INTERFACE_INFO: SyncValue<sys::spa_interface_info> = SyncValue(sys::spa_interface_info {
    type_: sys::SPA_TYPE_INTERFACE_Node.as_ptr().cast(),
});

/// Immutable SPA factory table for one concrete Rust [`Node`].
#[repr(transparent)]
pub struct Factory(SyncValue<sys::spa_handle_factory>);

impl Factory {
    /// Creates a process-static factory table.
    ///
    /// `name` must include one trailing nul byte and no interior nul byte.
    pub const fn new<N: Node>(name: &'static [u8]) -> Self {
        Self(SyncValue(sys::spa_handle_factory {
            version: sys::SPA_VERSION_HANDLE_FACTORY,
            name: name.as_ptr().cast(),
            info: ptr::null(),
            get_size: Some(factory_get_size::<N>),
            init: Some(factory_init::<N>),
            enum_interface_info: Some(factory_enum_interface_info),
        }))
    }

    /// Returns the raw immutable factory pointer expected by SPA loaders.
    pub const fn as_ptr(&'static self) -> *const sys::spa_handle_factory {
        &self.0.0
    }
}

fn ffi_result(operation: impl FnOnce() -> Result<i32, i32>) -> i32 {
    match catch_unwind(AssertUnwindSafe(operation)) {
        Ok(Ok(result)) => result,
        Ok(Err(error)) => error,
        Err(_) => -libc::EIO,
    }
}

unsafe fn instance_mut<'a, N: Node>(object: *mut c_void) -> Result<&'a mut Handle<N>, i32> {
    unsafe { object.cast::<Handle<N>>().as_mut() }.ok_or(-libc::EINVAL)
}

fn claim<N: Node>(instance: &Handle<N>) -> Result<CallbackGuard<'_, State<N>>, i32> {
    instance.state.claim()
}

unsafe fn for_each_node_event(
    hooks: *mut sys::spa_hook_list,
    mut operation: impl FnMut(&sys::spa_node_events, *mut c_void),
) {
    unsafe {
        let sentinel = ptr::addr_of_mut!((*hooks).list);
        let mut link = (*sentinel).next;
        while link != sentinel {
            let next = (*link).next;
            let hook = link.cast::<sys::spa_hook>();
            let events = (*hook).cb.funcs.cast::<sys::spa_node_events>();
            if let Some(events) = events.as_ref() {
                operation(events, (*hook).cb.data);
            }
            link = next;
        }
    }
}

unsafe fn emit_node_info(hooks: *mut sys::spa_hook_list, info: *const sys::spa_node_info) {
    unsafe {
        for_each_node_event(hooks, |events, data| {
            if let Some(callback) = events.info {
                callback(data, info);
            }
        });
    }
}

unsafe fn emit_port_info(
    hooks: *mut sys::spa_hook_list,
    key: PortRef,
    info: *const sys::spa_port_info,
) {
    unsafe {
        for_each_node_event(hooks, |events, data| {
            if let Some(callback) = events.port_info {
                callback(data, key.direction, key.id, info);
            }
        });
    }
}

unsafe fn emit_result_param(
    hooks: *mut sys::spa_hook_list,
    seq: i32,
    id: u32,
    index: u32,
    param: *mut sys::spa_pod,
) {
    let result = sys::spa_result_node_params {
        id,
        index,
        next: index + 1,
        param,
    };
    unsafe {
        for_each_node_event(hooks, |events, data| {
            if let Some(callback) = events.result {
                callback(
                    data,
                    seq,
                    0,
                    sys::SPA_RESULT_TYPE_NODE_PARAMS,
                    ptr::from_ref(&result).cast(),
                );
            }
        });
    }
}

unsafe fn emit_param(
    hooks: *mut sys::spa_hook_list,
    seq: i32,
    id: u32,
    index: u32,
    value: &Value,
    filter: *const sys::spa_pod,
) -> Result<bool, i32> {
    unsafe {
        pod::with_filtered_pod(value, filter, |param| {
            emit_result_param(hooks, seq, id, index, param);
        })
        .map(|result| result.is_some())
    }
}

unsafe extern "C" fn factory_get_size<N: Node>(
    _factory: *const sys::spa_handle_factory,
    _params: *const sys::spa_dict,
) -> usize {
    size_of::<Handle<N>>()
}

unsafe extern "C" fn factory_init<N: Node>(
    _factory: *const sys::spa_handle_factory,
    handle: *mut sys::spa_handle,
    info: *const sys::spa_dict,
    _support: *const sys::spa_support,
    _n_support: u32,
) -> i32 {
    ffi_result(|| unsafe {
        if handle.is_null() {
            return Err(-libc::EINVAL);
        }
        let instance = handle.cast::<Handle<N>>();
        ptr::write(
            instance,
            Handle {
                handle: sys::spa_handle {
                    version: sys::SPA_VERSION_HANDLE,
                    get_interface: Some(handle_get_interface::<N>),
                    clear: Some(handle_clear::<N>),
                },
                node: sys::spa_node {
                    iface: sys::spa_interface {
                        type_: sys::SPA_TYPE_INTERFACE_Node.as_ptr().cast(),
                        version: sys::SPA_VERSION_NODE,
                        cb: sys::spa_callbacks {
                            funcs: ptr::from_ref(&Methods::<N>::VALUE).cast(),
                            data: instance.cast(),
                        },
                    },
                },
                hooks: std::mem::zeroed(),
                state: ManuallyDrop::new(CallbackGate::new(State::new(N::new(info.as_ref())?))),
            },
        );
        sys::spa_hook_list_init(&mut (*instance).hooks);
        Ok(0)
    })
}

unsafe extern "C" fn factory_enum_interface_info(
    _factory: *const sys::spa_handle_factory,
    info: *mut *const sys::spa_interface_info,
    index: *mut u32,
) -> i32 {
    ffi_result(|| unsafe {
        if info.is_null() || index.is_null() {
            return Err(-libc::EINVAL);
        }
        if *index != 0 {
            return Ok(0);
        }
        *info = &INTERFACE_INFO.0;
        *index = 1;
        Ok(1)
    })
}

unsafe extern "C" fn handle_get_interface<N: Node>(
    handle: *mut sys::spa_handle,
    type_: *const c_char,
    interface: *mut *mut c_void,
) -> i32 {
    ffi_result(|| unsafe {
        if handle.is_null() || type_.is_null() || interface.is_null() {
            return Err(-libc::EINVAL);
        }
        let requested = CStr::from_ptr(type_);
        let node_type = CStr::from_bytes_with_nul(sys::SPA_TYPE_INTERFACE_Node)
            .expect("SPA node type is nul-terminated");
        if requested != node_type {
            return Err(-libc::ENOENT);
        }
        let instance = handle.cast::<Handle<N>>();
        *interface = ptr::addr_of_mut!((*instance).node).cast();
        Ok(0)
    })
}

unsafe extern "C" fn handle_clear<N: Node>(handle: *mut sys::spa_handle) -> i32 {
    ffi_result(|| unsafe {
        if handle.is_null() {
            return Err(-libc::EINVAL);
        }
        let instance = handle.cast::<Handle<N>>();
        ManuallyDrop::drop(&mut (*instance).state);
        Ok(0)
    })
}

unsafe extern "C" fn node_add_listener<N: Node>(
    object: *mut c_void,
    listener: *mut sys::spa_hook,
    events: *const sys::spa_node_events,
    data: *mut c_void,
) -> i32 {
    ffi_result(|| unsafe {
        let instance = instance_mut::<N>(object)?;
        if listener.is_null() || events.is_null() {
            return Err(-libc::EINVAL);
        }
        let (mut node_info, port_info) = {
            let mut state = claim(instance)?;
            let node_info = state.node_info();
            let ports = state.node.ports_mut();
            let port_info = ports
                .iter_mut()
                .map(|port| {
                    port.info.params = port.params.as_mut_ptr();
                    port.info.n_params = port.params.len() as u32;
                    (port.key, port.info)
                })
                .collect::<Vec<_>>();
            (node_info, port_info)
        };
        let mut saved = std::mem::zeroed();
        sys::spa_hook_list_isolate(
            &mut instance.hooks,
            &mut saved,
            listener,
            events.cast(),
            data,
        );
        node_info.change_mask =
            (sys::SPA_NODE_CHANGE_MASK_FLAGS | sys::SPA_NODE_CHANGE_MASK_PARAMS) as u64;
        emit_node_info(&mut instance.hooks, &node_info);
        for (key, mut info) in port_info {
            info.change_mask =
                (sys::SPA_PORT_CHANGE_MASK_FLAGS | sys::SPA_PORT_CHANGE_MASK_PARAMS) as u64;
            emit_port_info(&mut instance.hooks, key, &info);
        }
        sys::spa_hook_list_join(&mut instance.hooks, &mut saved);
        Ok(0)
    })
}

unsafe extern "C" fn node_set_callbacks<N: Node>(
    object: *mut c_void,
    _callbacks: *const sys::spa_node_callbacks,
    _data: *mut c_void,
) -> i32 {
    ffi_result(|| unsafe {
        instance_mut::<N>(object)?;
        Ok(0)
    })
}

unsafe extern "C" fn node_enum_params<N: Node>(
    object: *mut c_void,
    seq: i32,
    id: u32,
    start: u32,
    max: u32,
    filter: *const sys::spa_pod,
) -> i32 {
    ffi_result(|| unsafe {
        if max == 0 {
            return Err(-libc::EINVAL);
        }
        let instance = instance_mut::<N>(object)?;
        let mut index = start;
        let mut emitted = 0;
        while emitted < max {
            let value = {
                let state = claim(instance)?;
                state.node.enum_param(id, index)?
            };
            let Some(value) = value else {
                break;
            };
            if emit_param(&mut instance.hooks, seq, id, index, &value, filter)? {
                emitted += 1;
            }
            index += 1;
        }
        Ok(0)
    })
}

unsafe extern "C" fn node_set_param<N: Node>(
    object: *mut c_void,
    id: u32,
    flags: u32,
    param: *const sys::spa_pod,
) -> i32 {
    ffi_result(|| unsafe {
        let instance = instance_mut::<N>(object)?;
        let value = if param.is_null() {
            None
        } else {
            Some(pod::decode(param)?)
        };
        let mut state = claim(instance)?;
        let started = state.started;
        state.node.set_param(id, flags, value, started)?;
        Ok(0)
    })
}

unsafe extern "C" fn node_set_io<N: Node>(
    object: *mut c_void,
    _id: u32,
    _data: *mut c_void,
    _size: usize,
) -> i32 {
    ffi_result(|| unsafe {
        instance_mut::<N>(object)?;
        Err(-libc::ENOTSUP)
    })
}

unsafe extern "C" fn node_send_command<N: Node>(
    object: *mut c_void,
    command: *const sys::spa_command,
) -> i32 {
    ffi_result(|| unsafe {
        let instance = instance_mut::<N>(object)?;
        if command.is_null() {
            return Err(-libc::EINVAL);
        }
        let mut state = claim(instance)?;
        match sys::spa_node_command_id(command.cast_mut()) {
            sys::SPA_NODE_COMMAND_Start => {
                if state.started {
                    return Ok(0);
                }
                state.ready()?;
                state.node.start()?;
                let port_count = state.node.ports().len();
                for index in 0..port_count {
                    if let Err(error) = state.node.ports_mut()[index].worker_begin() {
                        for rollback in (0..index).rev() {
                            let _ = state.node.ports_mut()[rollback].worker_end();
                        }
                        state.node.pause();
                        return Err(error);
                    }
                }
                state.started = true;
                Ok(0)
            }
            sys::SPA_NODE_COMMAND_Pause => {
                if state.started {
                    state.started = false;
                    let mut worker_error = None;
                    for port in state.node.ports_mut().iter_mut().rev() {
                        if let Err(error) = port.worker_end() {
                            worker_error.get_or_insert(error);
                        }
                    }
                    state.node.pause();
                    if let Some(error) = worker_error {
                        return Err(error);
                    }
                }
                Ok(0)
            }
            _ => Err(-libc::ENOTSUP),
        }
    })
}

unsafe extern "C" fn node_add_port<N: Node>(
    object: *mut c_void,
    _direction: sys::spa_direction,
    _port_id: u32,
    _props: *const sys::spa_dict,
) -> i32 {
    ffi_result(|| unsafe {
        instance_mut::<N>(object)?;
        Err(-libc::ENOTSUP)
    })
}

unsafe extern "C" fn node_remove_port<N: Node>(
    object: *mut c_void,
    _direction: sys::spa_direction,
    _port_id: u32,
) -> i32 {
    ffi_result(|| unsafe {
        instance_mut::<N>(object)?;
        Err(-libc::ENOTSUP)
    })
}

fn port_index<N: Node>(node: &N, direction: sys::spa_direction, id: u32) -> Result<usize, i32> {
    node.ports()
        .iter()
        .position(|port| port.key == (PortRef { direction, id }))
        .ok_or(-libc::EINVAL)
}

unsafe extern "C" fn node_port_enum_params<N: Node>(
    object: *mut c_void,
    seq: i32,
    direction: sys::spa_direction,
    port_id: u32,
    id: u32,
    start: u32,
    max: u32,
    filter: *const sys::spa_pod,
) -> i32 {
    ffi_result(|| unsafe {
        if max == 0 {
            return Err(-libc::EINVAL);
        }
        let instance = instance_mut::<N>(object)?;
        let mut index = start;
        let mut emitted = 0;
        while emitted < max {
            let value = {
                let state = claim(instance)?;
                let port = &state.node.ports()[port_index(&state.node, direction, port_id)?];
                pod::port_param(
                    id,
                    index,
                    port.key.direction,
                    &port.constraints,
                    port.format.as_ref(),
                    port.latest_allowed(),
                )?
            };
            let Some(value) = value else {
                break;
            };
            if emit_param(&mut instance.hooks, seq, id, index, &value, filter)? {
                emitted += 1;
            }
            index += 1;
        }
        Ok(0)
    })
}

unsafe extern "C" fn node_port_set_param<N: Node>(
    object: *mut c_void,
    direction: sys::spa_direction,
    port_id: u32,
    id: u32,
    flags: u32,
    param: *const sys::spa_pod,
) -> i32 {
    ffi_result(|| unsafe {
        if id != sys::SPA_PARAM_Format {
            return Err(-libc::ENOENT);
        }
        let instance = instance_mut::<N>(object)?;
        let mut state = claim(instance)?;
        let index = port_index(&state.node, direction, port_id)?;
        let previous_node_flags = state.node_flags();
        let format = if param.is_null() {
            None
        } else {
            let value = pod::decode(param)?;
            Some(pod::parse_format(
                value,
                &state.node.ports()[index].constraints,
            )?)
        };
        if state.started {
            let current = state.node.ports()[index].format.as_ref();
            if current != format.as_ref() {
                return Err(-libc::EBUSY);
            }
        }
        state.node.validate_format(index, format.as_ref())?;
        if flags & sys::SPA_NODE_PARAM_FLAG_TEST_ONLY != 0 {
            return Ok(0);
        }
        if state.node.ports()[index].format != format {
            let previous = state.node.ports()[index].format.clone();
            let port = &mut state.node.ports_mut()[index];
            port.clear_buffers();
            port.format = format;
            port.update_format_params();
            if let Err(error) = state.node.format_changed(index) {
                let port = &mut state.node.ports_mut()[index];
                port.format = previous;
                port.update_format_params();
                let _ = state.node.format_changed(index);
                return Err(error);
            }
        }
        let (key, mut info, node_info) = {
            let port = &mut state.node.ports_mut()[index];
            port.info.params = port.params.as_mut_ptr();
            port.info.n_params = port.params.len() as u32;
            let key = port.key;
            let info = port.info;
            let node_info = (state.node_flags() != previous_node_flags).then(|| state.node_info());
            (key, info, node_info)
        };
        drop(state);
        info.change_mask = sys::SPA_PORT_CHANGE_MASK_PARAMS as u64;
        emit_port_info(&mut instance.hooks, key, &info);
        if let Some(mut info) = node_info {
            info.change_mask = sys::SPA_NODE_CHANGE_MASK_FLAGS as u64;
            emit_node_info(&mut instance.hooks, &info);
        }
        Ok(0)
    })
}

unsafe extern "C" fn node_port_use_buffers<N: Node>(
    object: *mut c_void,
    direction: sys::spa_direction,
    port_id: u32,
    flags: u32,
    buffers: *mut *mut sys::spa_buffer,
    n_buffers: u32,
) -> i32 {
    ffi_result(|| unsafe {
        let instance = instance_mut::<N>(object)?;
        let mut state = claim(instance)?;
        let index = port_index(&state.node, direction, port_id)?;
        if state.started {
            return Err(-libc::EBUSY);
        }
        if flags & sys::SPA_NODE_BUFFERS_FLAG_ALLOC != 0 {
            return Err(-libc::ENOTSUP);
        }
        if n_buffers == 0 {
            state.node.ports_mut()[index].clear_buffers();
            return Ok(0);
        }
        if buffers.is_null() || n_buffers as usize > MAX_BUFFERS {
            return Err(-libc::ENOSPC);
        }
        let minimum_size = state.node.ports()[index]
            .format
            .as_ref()
            .ok_or(-libc::EIO)?
            .packed_bytes()?;
        let supplied = std::slice::from_raw_parts(buffers, n_buffers as usize);
        for &buffer in supplied {
            let buffer = buffer.as_ref().ok_or(-libc::EINVAL)?;
            if buffer.n_datas != 1 || buffer.datas.is_null() {
                return Err(-libc::EINVAL);
            }
            let data = &*buffer.datas;
            if data.type_ != sys::SPA_DATA_MemPtr
                || data.data.is_null()
                || data.chunk.is_null()
                || (data.maxsize as usize) < minimum_size
            {
                return Err(-libc::EINVAL);
            }
        }
        let port = &mut state.node.ports_mut()[index];
        port.clear_buffers();
        for (buffer_index, &buffer) in supplied.iter().enumerate() {
            port.buffers[buffer_index].buffer = buffer;
            port.buffers[buffer_index].available = direction == sys::SPA_DIRECTION_OUTPUT;
        }
        port.n_buffers = supplied.len();
        port.update_latest_buffers();
        Ok(0)
    })
}

unsafe extern "C" fn node_port_set_io<N: Node>(
    object: *mut c_void,
    direction: sys::spa_direction,
    port_id: u32,
    id: u32,
    data: *mut c_void,
    size: usize,
) -> i32 {
    ffi_result(|| unsafe {
        let instance = instance_mut::<N>(object)?;
        let mut state = claim(instance)?;
        let index = port_index(&state.node, direction, port_id)?;
        let started = state.started;
        let port = &mut state.node.ports_mut()[index];
        if started && !port.is_latest_io(id) {
            return Err(-libc::EBUSY);
        }
        if started {
            port.worker_end()?;
        }
        if let Err(error) = port.set_io(id, data, size) {
            if started {
                let _ = port.worker_begin();
            }
            return Err(error);
        }
        if started {
            port.worker_begin()?;
        }
        Ok(0)
    })
}

unsafe extern "C" fn node_port_reuse_buffer<N: Node>(
    object: *mut c_void,
    port_id: u32,
    buffer_id: u32,
) -> i32 {
    ffi_result(|| unsafe {
        let instance = instance_mut::<N>(object)?;
        let mut state = claim(instance)?;
        let index = port_index(&state.node, sys::SPA_DIRECTION_OUTPUT, port_id)?;
        let port = &mut state.node.ports_mut()[index];
        if buffer_id as usize >= port.n_buffers {
            return Err(-libc::EINVAL);
        }
        port.buffers[buffer_id as usize].available = true;
        Ok(0)
    })
}

unsafe extern "C" fn node_process<N: Node>(object: *mut c_void) -> i32 {
    ffi_result(|| unsafe {
        let instance = instance_mut::<N>(object)?;
        let mut state = claim(instance)?;
        if !state.started {
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        }
        state.node.process()
    })
}

#[cfg(test)]
mod tests {
    use super::{CallbackGate, ffi_result};

    #[test]
    fn callback_gate_never_waits_for_an_active_callback() {
        let gate = CallbackGate::new(7);
        let mut guard = gate.claim().expect("first callback claims the node");
        *guard += 1;
        assert!(matches!(gate.claim(), Err(error) if error == -libc::EBUSY));
        drop(guard);
        assert_eq!(*gate.claim().expect("released node can be reclaimed"), 8);
    }

    #[test]
    fn callback_panics_are_contained_as_io_errors() {
        assert_eq!(ffi_result(|| panic!("test callback panic")), -libc::EIO);
    }
}
