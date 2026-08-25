//! Rust implementation boundary for loadable Calculon SPA nodes.
//!
//! The crate owns SPA factories, node and port callbacks, POD formats, buffer
//! ownership, and complete-frame scheduling. Numerical operations remain in
//! their owning algorithm or rendering crates.

#![allow(unsafe_code)]

mod buffer;
mod factory;
mod format;
mod latest;
mod pod;
mod port;

pub use buffer::{
    Header, InputFrame, OutputFrame, ProgressiveInputFrame, ProgressiveInputObservation, overlaps,
};
pub use factory::{Factory, Node};
pub use format::{Format, FormatClass, FormatConstraint, Rate};
pub use libspa::buffer::meta::{ProgressiveFlags, ProgressiveState};
pub use libspa::pod::Value as PodValue;
pub use libspa::sys;
pub use pod::{Property, object, parse_props};
pub use port::{Direction, Port, PortRef};
