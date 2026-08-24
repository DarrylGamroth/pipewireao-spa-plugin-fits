//! Fused Shack-Hartmann reconstruction and bounded physical-DM control.

use std::sync::Arc;

use calculon_algorithms::schemas::{CALIBRATED_PIXELS_V1, DEMANDED_PDM_COMMAND_V1};
use calculon_algorithms::{
    AlgorithmPlan, LeakyIntegratorPlan, LeakyIntegratorWorkspace, PdmCommandInput,
    PdmCommandOutput, PdmCommandPlan, PdmCommandWorkspace, PreparedGemv, ProgressWorkspace,
    RegionExtractionPlan, RegionPixels, RegionPixelsMut, ShackHartmannOutput, ShackHartmannPlan,
};
use calculon_spa_node::{
    Factory, Format, FormatConstraint, InputFrame, Node, OutputFrame, Port, PortRef, sys,
};

use crate::config::{
    parse_finite_f32, parse_positive_usize, parse_rate, parse_size, required_info, valid_profile,
};

/// Factory name of the fused Shack-Hartmann controller node.
pub const SHWFS_CONTROLLER_FACTORY_NAME: &str = "api.calculon.shwfs-controller";

const KEY_SIZE: &[u8] = b"api.calculon.detector-size\0";
const KEY_RATE: &[u8] = b"api.calculon.detector-rate\0";
const KEY_PROFILE: &[u8] = b"api.calculon.detector-profile\0";
const KEY_REGION_SIZE: &[u8] = b"api.calculon.region-size\0";
const KEY_REGION_ORIGINS: &[u8] = b"api.calculon.region-origins\0";
const KEY_ACTUATOR_COUNT: &[u8] = b"api.calculon.actuator-count\0";
const KEY_MATRIX_PATH: &[u8] = b"api.calculon.reconstruction-matrix-path\0";
const KEY_COORDINATE_SCALE: &[u8] = b"api.calculon.coordinate-scale\0";
const KEY_PIXEL_THRESHOLD: &[u8] = b"api.calculon.pixel-threshold\0";
const KEY_FLUX_THRESHOLD: &[u8] = b"api.calculon.flux-threshold\0";
const KEY_CONTROLLER_GAIN: &[u8] = b"api.calculon.controller-gain\0";
const KEY_CONTROLLER_POLE: &[u8] = b"api.calculon.controller-pole\0";
const KEY_COMMAND_MINIMUM: &[u8] = b"api.calculon.command-minimum\0";
const KEY_COMMAND_MAXIMUM: &[u8] = b"api.calculon.command-maximum\0";

const INPUT: usize = 0;
const OUTPUT: usize = 1;

pub(crate) static FACTORY: Factory =
    Factory::new::<ShwfsControllerNode>(b"api.calculon.shwfs-controller\0");

struct GradientCoordinates {
    x: Arc<[f32]>,
    y: Arc<[f32]>,
}

struct ShwfsControllerNode {
    ports: Vec<Port>,
    width: usize,
    height: usize,
    region_count: usize,
    pixels_per_region: usize,
    actuator_count: usize,
    extraction: RegionExtractionPlan<f32>,
    extraction_workspace: ProgressWorkspace,
    shack_hartmann: ShackHartmannPlan<f32>,
    shack_hartmann_workspace: ProgressWorkspace,
    reconstructor: PreparedGemv<f32>,
    integrator: LeakyIntegratorPlan<f32>,
    integrator_workspace: LeakyIntegratorWorkspace<f32>,
    command: PdmCommandPlan<f32>,
    command_workspace: PdmCommandWorkspace<f32>,
    image: Box<[f32]>,
    region_pixels: Box<[f32]>,
    slopes: Box<[f32]>,
    flux: Box<[f32]>,
    validity: Box<[bool]>,
    reconstructed: Box<[f32]>,
    requested: Box<[f32]>,
    demanded: Box<[f32]>,
    constraint_feedback: Box<[f32]>,
}

impl Node for ShwfsControllerNode {
    fn new(info: Option<&sys::spa_dict>) -> Result<Self, i32> {
        let (width, height) = parse_size(required_info(info, KEY_SIZE)?)?;
        let rate = parse_rate(required_info(info, KEY_RATE)?)?;
        let profile = required_info(info, KEY_PROFILE)?;
        if !valid_profile(profile) {
            return Err(-libc::EINVAL);
        }
        let (region_width, region_height) = parse_size(required_info(info, KEY_REGION_SIZE)?)?;
        let origins = parse_origins(required_info(info, KEY_REGION_ORIGINS)?)?;
        let actuator_count = parse_positive_usize(required_info(info, KEY_ACTUATOR_COUNT)?)?;
        let coordinate_scale = parse_finite_f32(required_info(info, KEY_COORDINATE_SCALE)?)?;
        if coordinate_scale <= 0.0 {
            return Err(-libc::EINVAL);
        }
        let pixel_threshold = parse_finite_f32(required_info(info, KEY_PIXEL_THRESHOLD)?)?;
        let flux_threshold = parse_finite_f32(required_info(info, KEY_FLUX_THRESHOLD)?)?;
        if pixel_threshold < 0.0 || flux_threshold < 0.0 {
            return Err(-libc::EINVAL);
        }
        let gain = parse_finite_f32(required_info(info, KEY_CONTROLLER_GAIN)?)?;
        let pole = parse_finite_f32(required_info(info, KEY_CONTROLLER_POLE)?)?;
        let command_minimum = parse_finite_f32(required_info(info, KEY_COMMAND_MINIMUM)?)?;
        let command_maximum = parse_finite_f32(required_info(info, KEY_COMMAND_MAXIMUM)?)?;
        if command_minimum > command_maximum {
            return Err(-libc::EINVAL);
        }

        let extraction = RegionExtractionPlan::new(
            (height as usize, width as usize),
            &origins,
            (region_height as usize, region_width as usize),
        )
        .map_err(|_| -libc::EINVAL)?;
        let region_count = extraction.len();
        let pixels_per_region = extraction.pixels_per_region();
        let coordinates = centered_coordinates(
            region_width as usize,
            region_height as usize,
            coordinate_scale,
        )?;
        let slope_count = region_count.checked_mul(2).ok_or(-libc::EOVERFLOW)?;
        let shack_hartmann = ShackHartmannPlan::new(
            coordinates.x,
            coordinates.y,
            Arc::<[f32]>::from(vec![0.0; slope_count]),
            Arc::<[f32]>::from(vec![pixel_threshold; region_count]),
            Arc::<[f32]>::from(vec![flux_threshold; region_count]),
            Arc::<[bool]>::from(vec![true; region_count]),
        )
        .map_err(|_| -libc::EINVAL)?;
        let matrix = read_f32_matrix(
            required_info(info, KEY_MATRIX_PATH)?,
            actuator_count,
            slope_count,
        )?;
        let reconstructor =
            PreparedGemv::new(actuator_count, slope_count, matrix).map_err(|_| -libc::EINVAL)?;
        let integrator = LeakyIntegratorPlan::new(gain, pole).map_err(|_| -libc::EINVAL)?;
        let integrator_workspace =
            LeakyIntegratorWorkspace::new(actuator_count).map_err(|_| -libc::EINVAL)?;
        let command = PdmCommandPlan::new(
            Arc::<[f32]>::from(vec![command_minimum; actuator_count]),
            Arc::<[f32]>::from(vec![command_maximum; actuator_count]),
            None,
        )
        .map_err(|_| -libc::EINVAL)?;
        let command_workspace = command
            .workspace(&vec![0.0; actuator_count])
            .map_err(|_| -libc::EINVAL)?;
        let input_format =
            Format::f32_image(CALIBRATED_PIXELS_V1, profile, width, height, Some(rate))?;
        let output_format = Format::ndarray(
            sys::SPA_ELEMENT_TYPE_F32_LE,
            DEMANDED_PDM_COMMAND_V1,
            profile,
            vec![u32::try_from(actuator_count).map_err(|_| -libc::EOVERFLOW)?],
            Some(rate),
        )?;
        let image_length = (width as usize)
            .checked_mul(height as usize)
            .ok_or(-libc::EOVERFLOW)?;
        let region_length = region_count
            .checked_mul(pixels_per_region)
            .ok_or(-libc::EOVERFLOW)?;

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
            width: width as usize,
            height: height as usize,
            region_count,
            pixels_per_region,
            actuator_count,
            extraction,
            extraction_workspace: ProgressWorkspace::new(),
            shack_hartmann,
            shack_hartmann_workspace: ProgressWorkspace::new(),
            reconstructor,
            integrator,
            integrator_workspace,
            command,
            command_workspace,
            image: vec![0.0; image_length].into_boxed_slice(),
            region_pixels: vec![0.0; region_length].into_boxed_slice(),
            slopes: vec![0.0; slope_count].into_boxed_slice(),
            flux: vec![0.0; region_count].into_boxed_slice(),
            validity: vec![false; region_count].into_boxed_slice(),
            reconstructed: vec![0.0; actuator_count].into_boxed_slice(),
            requested: vec![0.0; actuator_count].into_boxed_slice(),
            demanded: vec![0.0; actuator_count].into_boxed_slice(),
            constraint_feedback: vec![0.0; actuator_count].into_boxed_slice(),
        })
    }

    fn ports(&self) -> &[Port] {
        &self.ports
    }

    fn ports_mut(&mut self) -> &mut [Port] {
        &mut self.ports
    }

    fn ready(&self) -> Result<(), i32> {
        if self.reconstructor.rows() != self.actuator_count
            || self.reconstructor.columns() != self.region_count * 2
            || self.region_pixels.len() != self.region_count * self.pixels_per_region
        {
            return Err(-libc::EIO);
        }
        Ok(())
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
            let header = {
                let source = InputFrame::new(input_buffer, input_format)?;
                let (pixels, stride) = source.f32()?;
                for row in 0..self.height {
                    let source_start = row * stride;
                    let source_row = pixels
                        .get(source_start..source_start + self.width)
                        .ok_or(-libc::EINVAL)?;
                    if source_row.iter().any(|value| !value.is_finite()) {
                        return Err(-libc::EDOM);
                    }
                    self.image[row * self.width..(row + 1) * self.width]
                        .copy_from_slice(source_row);
                }
                source.header()
            };
            self.extraction
                .process(
                    RegionPixelsMut::new(
                        &mut self.region_pixels,
                        self.pixels_per_region,
                        self.region_count,
                    )
                    .map_err(|_| -libc::EIO)?,
                    &mut self.extraction_workspace,
                    &self.image,
                )
                .map_err(|_| -libc::EIO)?;
            self.shack_hartmann
                .process(
                    ShackHartmannOutput::new(&mut self.slopes, &mut self.flux, &mut self.validity)
                        .map_err(|_| -libc::EIO)?,
                    &mut self.shack_hartmann_workspace,
                    RegionPixels::new(
                        &self.region_pixels,
                        self.pixels_per_region,
                        self.region_count,
                    )
                    .map_err(|_| -libc::EIO)?,
                )
                .map_err(|_| -libc::EIO)?;
            self.reconstructor
                .multiply(&mut self.reconstructed, &self.slopes)
                .map_err(|_| -libc::EDOM)?;
            self.integrator
                .process(
                    &mut self.requested,
                    &mut self.integrator_workspace,
                    &self.reconstructed,
                )
                .map_err(|_| -libc::EIO)?;
            self.command
                .process(
                    PdmCommandOutput {
                        demanded: &mut self.demanded,
                        constraint_feedback: &mut self.constraint_feedback,
                    },
                    &mut self.command_workspace,
                    PdmCommandInput {
                        requested: &self.requested,
                    },
                )
                .map_err(|_| -libc::EIO)?;
            let mut destination = OutputFrame::new(output_buffer, output_format)?;
            let (commands, stride) = destination.f32_mut()?;
            if stride != 1 || commands.len() != self.actuator_count {
                return Err(-libc::EIO);
            }
            commands.copy_from_slice(&self.demanded);
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

fn parse_origins(value: &str) -> Result<Vec<(usize, usize)>, i32> {
    if value.is_empty() {
        return Err(-libc::EINVAL);
    }
    value
        .split(';')
        .map(|origin| {
            let (row, column) = origin.split_once(',').ok_or(-libc::EINVAL)?;
            Ok((
                row.parse::<usize>().map_err(|_| -libc::EINVAL)?,
                column.parse::<usize>().map_err(|_| -libc::EINVAL)?,
            ))
        })
        .collect()
}

fn centered_coordinates(
    width: usize,
    height: usize,
    scale: f32,
) -> Result<GradientCoordinates, i32> {
    let length = width.checked_mul(height).ok_or(-libc::EOVERFLOW)?;
    let mut x = Vec::with_capacity(length);
    let mut y = Vec::with_capacity(length);
    let x_center = (width as f32 - 1.0) * 0.5;
    let y_center = (height as f32 - 1.0) * 0.5;
    for row in 0..height {
        for column in 0..width {
            x.push((column as f32 - x_center) * scale);
            y.push((row as f32 - y_center) * scale);
        }
    }
    Ok(GradientCoordinates {
        x: Arc::from(x),
        y: Arc::from(y),
    })
}

fn read_f32_matrix(path: &str, rows: usize, columns: usize) -> Result<Arc<[f32]>, i32> {
    let elements = rows.checked_mul(columns).ok_or(-libc::EOVERFLOW)?;
    let bytes = std::fs::read(path).map_err(|error| -error.raw_os_error().unwrap_or(libc::EIO))?;
    if bytes.len() != elements.checked_mul(4).ok_or(-libc::EOVERFLOW)? {
        return Err(-libc::EINVAL);
    }
    let values = bytes
        .chunks_exact(4)
        .map(|chunk| f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
        .collect::<Vec<_>>();
    if values.iter().any(|value| !value.is_finite()) {
        return Err(-libc::EDOM);
    }
    Ok(Arc::from(values))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn origins_preserve_region_order() {
        assert_eq!(parse_origins("0,1;4,5"), Ok(vec![(0, 1), (4, 5)]));
        assert_eq!(parse_origins(""), Err(-libc::EINVAL));
        assert_eq!(parse_origins("0:1"), Err(-libc::EINVAL));
    }

    #[test]
    fn centered_coordinates_follow_detector_axes() {
        let coordinates = centered_coordinates(2, 2, 2.0).unwrap();
        assert_eq!(&*coordinates.x, &[-1.0, 1.0, -1.0, 1.0]);
        assert_eq!(&*coordinates.y, &[-1.0, -1.0, 1.0, 1.0]);
    }
}
