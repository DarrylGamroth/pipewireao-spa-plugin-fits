//! Profile-bound conversion from physical PDM commands to ALPAO SDK values.

use calculon_algorithms::schemas::DEMANDED_PDM_COMMAND_V1;
use calculon_spa_node::{
    Factory, Format, FormatConstraint, InputFrame, Node, OutputFrame, Port, PortRef, sys,
};

use crate::config::{
    parse_finite_f32, parse_positive_usize, parse_rate, required_info, valid_profile,
};

/// Factory name of the ALPAO physical-command normalization node.
pub const ALPAO_COMMAND_NORMALIZATION_FACTORY_NAME: &str = "api.alpao.command-normalization";

/// Semantic schema consumed by `api.alpao.sink`.
pub const ALPAO_NORMALIZED_ACTUATOR_COMMAND_V1: &str =
    "org.pipewireao.alpao.normalized-actuator-command/1";

const KEY_RATE: &[u8] = b"api.calculon.detector-rate\0";
const KEY_PROFILE: &[u8] = b"api.alpao.profile\0";
const KEY_ACTUATOR_COUNT: &[u8] = b"api.alpao.actuator-count\0";
const KEY_COMMAND_SCALE: &[u8] = b"api.alpao.command-scale\0";
const INPUT: usize = 0;
const OUTPUT: usize = 1;

pub(crate) static FACTORY: Factory =
    Factory::new::<AlpaoCommandNormalizationNode>(b"api.alpao.command-normalization\0");

struct AlpaoCommandNormalizationNode {
    ports: Vec<Port>,
    actuator_count: usize,
    command_scale: f64,
}

impl Node for AlpaoCommandNormalizationNode {
    fn new(info: Option<&sys::spa_dict>) -> Result<Self, i32> {
        let rate = parse_rate(required_info(info, KEY_RATE)?)?;
        let profile = required_info(info, KEY_PROFILE)?;
        if !valid_profile(profile) {
            return Err(-libc::EINVAL);
        }
        let actuator_count = parse_positive_usize(required_info(info, KEY_ACTUATOR_COUNT)?)?;
        let command_scale = parse_finite_f32(required_info(info, KEY_COMMAND_SCALE)?)?;
        if command_scale <= 0.0 {
            return Err(-libc::EINVAL);
        }
        let shape = vec![u32::try_from(actuator_count).map_err(|_| -libc::EOVERFLOW)?];
        let input_format = Format::ndarray(
            sys::SPA_ELEMENT_TYPE_F32_LE,
            DEMANDED_PDM_COMMAND_V1,
            profile,
            shape.clone(),
            Some(rate),
        )?;
        let output_format = Format::ndarray(
            sys::SPA_ELEMENT_TYPE_F64_LE,
            ALPAO_NORMALIZED_ACTUATOR_COMMAND_V1,
            profile,
            shape,
            None,
        )?;
        Ok(Self {
            ports: vec![
                Port::new(
                    PortRef {
                        direction: sys::SPA_DIRECTION_INPUT,
                        id: 0,
                    },
                    true,
                    false,
                    [FormatConstraint::exact(input_format)],
                ),
                Port::new(
                    PortRef {
                        direction: sys::SPA_DIRECTION_OUTPUT,
                        id: 0,
                    },
                    true,
                    false,
                    [FormatConstraint::exact(output_format)],
                ),
            ],
            actuator_count,
            command_scale: command_scale as f64,
        })
    }

    fn ports(&self) -> &[Port] {
        &self.ports
    }

    fn ports_mut(&mut self) -> &mut [Port] {
        &mut self.ports
    }

    fn process(&mut self) -> Result<i32, i32> {
        let (inputs, outputs) = self.ports.split_at_mut(OUTPUT);
        let input = &mut inputs[INPUT];
        let output = &mut outputs[0];
        if output.output_pending()? {
            return Ok(sys::SPA_STATUS_HAVE_DATA as i32);
        }
        let Some((_input_id, input_buffer)) = input.input_buffer()? else {
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        };
        let Some((output_id, output_buffer)) = output.reserve_output()? else {
            return Ok(sys::SPA_STATUS_HAVE_DATA as i32);
        };
        let input_format = input.format().ok_or(-libc::EIO)?;
        let output_format = output.format().ok_or(-libc::EIO)?;
        let result = (|| unsafe {
            let source = InputFrame::new(input_buffer, input_format)?;
            let header = source.header();
            let (physical, physical_stride) = source.f32()?;
            if physical_stride != 1 || physical.len() != self.actuator_count {
                return Err(-libc::EIO);
            }
            let mut destination = OutputFrame::new(output_buffer, output_format)?;
            let (normalized, normalized_stride) = destination.f64_mut()?;
            if normalized_stride != 1 || normalized.len() != self.actuator_count {
                return Err(-libc::EIO);
            }
            for (destination, &source) in normalized.iter_mut().zip(physical) {
                let value = f64::from(source) / self.command_scale;
                if !value.is_finite() || !(-1.0..=1.0).contains(&value) {
                    return Err(-libc::ERANGE);
                }
                *destination = value;
            }
            destination.set_header(header);
            destination.commit();
            Ok::<(), i32>(())
        })();
        if let Err(error) = result {
            output.cancel_output(output_id)?;
            input.reject_input(error)?;
            return Err(error);
        }
        input.consume_input()?;
        output.publish_output(output_id)?;
        Ok(sys::SPA_STATUS_HAVE_DATA as i32)
    }
}
