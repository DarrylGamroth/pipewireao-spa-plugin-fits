//! Complete-frame detector calibration with independently prepared planes.

use std::sync::Arc;

use calculon_algorithms::schemas::{
    BACKGROUND_CALIBRATION_V1, CALIBRATED_PIXELS_V1, FLAT_CALIBRATION_V1,
};
use calculon_algorithms::{AlgorithmPlan, InputProgress, PixelCalibrationPlan, ProgressWorkspace};
use calculon_spa_node::{
    Factory, Format, FormatConstraint, Header, InputFrame, Node, OutputFrame, PodValue, Port,
    PortRef, ProgressiveInputFrame, ProgressiveState, Property, object, parse_props, sys,
};
use libspa::utils::Id;

use crate::CALIBRATED_PIXEL_ROW_BLOCK_V1;
use crate::config::{
    optional_info, parse_positive_usize, parse_rate, parse_size, required_info, valid_profile,
};

/// Factory name of the complete-frame detector-calibration node.
pub const PIXEL_CALIBRATION_FACTORY_NAME: &str = "api.calculon.pixel-calibration";

const KEY_SIZE: &[u8] = b"api.calculon.detector-size\0";
const KEY_RATE: &[u8] = b"api.calculon.detector-rate\0";
const KEY_PROFILE: &[u8] = b"api.calculon.detector-profile\0";
const KEY_BLOCK_ROWS: &[u8] = b"api.calculon.row-block-rows\0";
const PROP_FLAT: u32 = sys::SPA_PROP_START_CUSTOM;
const PROP_BACKGROUND: u32 = sys::SPA_PROP_START_CUSTOM + 1;
const INPUT_RAW: usize = 0;
const INPUT_FLAT: usize = 1;
const INPUT_BACKGROUND: usize = 2;
const OUTPUT: usize = 3;
const SLOT_COUNT: usize = 3;

pub(crate) static FACTORY: Factory =
    Factory::new::<PixelCalibrationNode>(b"api.calculon.pixel-calibration\0");

struct PlaneSlot {
    seq: Option<u64>,
    values: Arc<[f32]>,
    age: u64,
}

struct PlaneStore {
    slots: [PlaneSlot; SLOT_COUNT],
    age: u64,
}

impl PlaneStore {
    fn new(length: usize) -> Self {
        Self {
            slots: std::array::from_fn(|_| PlaneSlot {
                seq: None,
                values: Arc::from(vec![0.0; length]),
                age: 0,
            }),
            age: 0,
        }
    }

    fn get(&self, seq: u64) -> Option<Arc<[f32]>> {
        self.slots
            .iter()
            .find(|slot| slot.seq == Some(seq))
            .map(|slot| Arc::clone(&slot.values))
    }

    fn ingest(
        &mut self,
        seq: u64,
        values: &[f32],
        width: usize,
        height: usize,
        stride: usize,
    ) -> Result<(), i32> {
        if seq > i64::MAX as u64 {
            return Err(-libc::ERANGE);
        }
        let length = width.checked_mul(height).ok_or(-libc::EOVERFLOW)?;
        if let Some(slot) = self.slots.iter().find(|slot| slot.seq == Some(seq)) {
            if slot.values.len() != length {
                return Err(-libc::EIO);
            }
            for row in 0..height {
                let start = row.checked_mul(stride).ok_or(-libc::EOVERFLOW)?;
                let end = start.checked_add(width).ok_or(-libc::EOVERFLOW)?;
                let source = values.get(start..end).ok_or(-libc::EINVAL)?;
                if source.iter().any(|value| !value.is_finite()) {
                    return Err(-libc::EDOM);
                }
                if slot.values[row * width..(row + 1) * width] != *source {
                    return Err(-libc::EEXIST);
                }
            }
            return Ok(());
        }
        let index = self
            .slots
            .iter()
            .enumerate()
            .filter(|(_, slot)| Arc::strong_count(&slot.values) == 1)
            .min_by_key(|(_, slot)| (slot.seq.is_some(), slot.age))
            .map(|(index, _)| index)
            .ok_or(-libc::ENOSPC)?;
        let destination = Arc::get_mut(&mut self.slots[index].values).ok_or(-libc::EBUSY)?;
        if destination.len() != length {
            return Err(-libc::EINVAL);
        }
        for row in 0..height {
            let start = row.checked_mul(stride).ok_or(-libc::EOVERFLOW)?;
            let end = start.checked_add(width).ok_or(-libc::EOVERFLOW)?;
            let source = values.get(start..end).ok_or(-libc::EINVAL)?;
            if source.iter().any(|value| !value.is_finite()) {
                return Err(-libc::EDOM);
            }
            destination[row * width..(row + 1) * width].copy_from_slice(source);
        }
        self.age = self.age.wrapping_add(1);
        self.slots[index].age = self.age;
        self.slots[index].seq = Some(seq);
        Ok(())
    }
}

struct PixelCalibrationNode {
    ports: Vec<Port>,
    width: usize,
    height: usize,
    block_rows: usize,
    flats: PlaneStore,
    backgrounds: PlaneStore,
    identity_flat: Arc<[f32]>,
    identity_background: Arc<[f32]>,
    flat: Arc<[f32]>,
    background: Arc<[f32]>,
    flat_seq: Option<u64>,
    background_seq: Option<u64>,
    plan: PixelCalibrationPlan<f32, u16>,
    active_plan: Option<PixelCalibrationPlan<f32, u16>>,
    workspace: ProgressWorkspace,
    raw: Box<[u16]>,
    calibrated: Box<[f32]>,
    next_row: usize,
    active_seq: Option<u64>,
    discontinuity: bool,
}

impl PixelCalibrationNode {
    fn ingest(&mut self, port: usize) -> Result<(), i32> {
        let Some(format) = self.ports[port].format().map(std::ptr::from_ref::<Format>) else {
            return Ok(());
        };
        let Some((_id, buffer)) = self.ports[port].input_buffer()? else {
            return Ok(());
        };
        let result = (|| unsafe {
            // Formats cannot change while the owning node callback gate is held.
            let source = InputFrame::new(buffer, &*format)?;
            let seq = source.header().ok_or(-libc::ENODATA)?.seq;
            let (values, stride) = source.f32()?;
            let store = match port {
                INPUT_FLAT => &mut self.flats,
                INPUT_BACKGROUND => &mut self.backgrounds,
                _ => return Err(-libc::EINVAL),
            };
            store.ingest(seq, values, self.width, self.height, stride)
        })();
        match result {
            Ok(()) => self.ports[port].consume_input(),
            Err(error) => {
                self.ports[port].reject_input(error)?;
                Err(error)
            }
        }
    }

    fn selected_plane(&self, port: usize, seq: Option<u64>) -> Result<Arc<[f32]>, i32> {
        match (port, seq) {
            (INPUT_FLAT, None) => Ok(Arc::clone(&self.identity_flat)),
            (INPUT_BACKGROUND, None) => Ok(Arc::clone(&self.identity_background)),
            (INPUT_FLAT, Some(seq)) if self.flat_seq == Some(seq) => Ok(Arc::clone(&self.flat)),
            (INPUT_BACKGROUND, Some(seq)) if self.background_seq == Some(seq) => {
                Ok(Arc::clone(&self.background))
            }
            (INPUT_FLAT, Some(seq)) => self.flats.get(seq).ok_or(-libc::ENOENT),
            (INPUT_BACKGROUND, Some(seq)) => self.backgrounds.get(seq).ok_or(-libc::ENOENT),
            _ => Err(-libc::EINVAL),
        }
    }

    fn reset_progressive(&mut self) {
        self.active_plan = None;
        self.active_seq = None;
        self.next_row = 0;
    }

    fn process_progressive(&mut self) -> Result<i32, i32> {
        let (inputs, outputs) = self.ports.split_at_mut(OUTPUT);
        let input = &mut inputs[INPUT_RAW];
        let output = &mut outputs[0];
        if output.output_pending()? {
            return Ok(sys::SPA_STATUS_HAVE_DATA as i32);
        }
        let Some((_input_id, input_buffer)) = input.input_buffer()? else {
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        };
        let input_format = input.format().ok_or(-libc::EIO)?;
        let progressive = unsafe { ProgressiveInputFrame::new(input_buffer, input_format)? };
        let observation = progressive.observe()?;
        let header = observation.header().ok_or(-libc::ENODATA)?;

        if self.active_seq.is_none() {
            if observation.state() == ProgressiveState::Aborted {
                input.consume_input()?;
                self.discontinuity = true;
                return Ok(sys::SPA_STATUS_NEED_DATA as i32);
            }
            self.plan
                .start(&mut self.workspace)
                .map_err(|_| -libc::EINVAL)?;
            self.active_plan = Some(self.plan.clone());
            self.active_seq = Some(header.seq);
        } else if self.active_seq != Some(header.seq) {
            let _ = self
                .active_plan
                .as_ref()
                .ok_or(-libc::EIO)?
                .abort(&mut self.workspace);
            self.active_plan = None;
            self.active_seq = None;
            self.next_row = 0;
            input.reject_input(-libc::EPROTO)?;
            self.discontinuity = true;
            return Err(-libc::EPROTO);
        }

        if observation.state() == ProgressiveState::Aborted {
            self.active_plan
                .as_ref()
                .ok_or(-libc::EIO)?
                .abort(&mut self.workspace)
                .map_err(|_| -libc::EINVAL)?;
            self.active_plan = None;
            self.active_seq = None;
            self.next_row = 0;
            input.consume_input()?;
            self.discontinuity = true;
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        }
        if observation.state() == ProgressiveState::Prepared {
            return Err(-libc::EPROTO);
        }

        let end_row = self
            .next_row
            .checked_add(self.block_rows)
            .ok_or(-libc::EOVERFLOW)?;
        if observation.complete_rows() < end_row
            || (end_row == self.height && observation.state() != ProgressiveState::Complete)
        {
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        }
        if end_row > self.height {
            return Err(-libc::EPROTO);
        }
        let Some((output_id, output_buffer)) = output.reserve_output()? else {
            return Ok(sys::SPA_STATUS_HAVE_DATA as i32);
        };
        let output_format = output.format().ok_or(-libc::EIO)?;
        let start_row = self.next_row;
        let result = (|| unsafe {
            let (raw, source_stride) = observation.u16();
            for row in start_row..end_row {
                let source_start = row * source_stride;
                let destination_start = row * self.width;
                self.raw[destination_start..destination_start + self.width]
                    .copy_from_slice(&raw[source_start..source_start + self.width]);
            }
            let mut destination = OutputFrame::new(output_buffer, output_format)?;
            let (calibrated, destination_stride) = destination.f32_mut()?;
            if destination_stride != self.width {
                return Err(-libc::EINVAL);
            }
            let pixel_range = start_row * self.width..end_row * self.width;
            let expected = pixel_range.end;
            let progress = self
                .active_plan
                .as_ref()
                .ok_or(-libc::EIO)?
                .process_range_window(
                    &mut calibrated[..self.block_rows * self.width],
                    &mut self.workspace,
                    &self.raw,
                    pixel_range,
                )
                .map_err(|_| -libc::EINVAL)?;
            if progress.len() != expected {
                return Err(-libc::EIO);
            }
            let mut block_header = Header {
                offset: u32::try_from(start_row).map_err(|_| -libc::EOVERFLOW)?,
                ..header
            };
            if self.discontinuity {
                block_header.flags |= sys::SPA_META_HEADER_FLAG_DISCONT;
            }
            if end_row == self.height {
                block_header.flags |= sys::SPA_META_HEADER_FLAG_MARKER;
            } else {
                block_header.flags &= !sys::SPA_META_HEADER_FLAG_MARKER;
            }
            destination.set_header(Some(block_header));
            destination.commit();
            Ok::<(), i32>(())
        })();
        if let Err(error) = result {
            output.cancel_output(output_id)?;
            let _ = self
                .active_plan
                .as_ref()
                .map(|plan| plan.abort(&mut self.workspace));
            self.active_plan = None;
            self.active_seq = None;
            self.next_row = 0;
            input.reject_input(error)?;
            self.discontinuity = true;
            return Err(error);
        }
        self.next_row = end_row;
        self.discontinuity = false;
        if end_row == self.height {
            self.active_plan
                .as_ref()
                .ok_or(-libc::EIO)?
                .finish(&mut [], &mut self.workspace)
                .map_err(|_| -libc::EINVAL)?;
            self.active_plan = None;
            self.active_seq = None;
            self.next_row = 0;
            input.consume_input()?;
        }
        output.publish_output(output_id)?;
        Ok(sys::SPA_STATUS_HAVE_DATA as i32)
    }
}

impl Node for PixelCalibrationNode {
    const HAS_PROPS: bool = true;

    fn new(info: Option<&sys::spa_dict>) -> Result<Self, i32> {
        let (width, height) = parse_size(required_info(info, KEY_SIZE)?)?;
        let rate = parse_rate(required_info(info, KEY_RATE)?)?;
        let profile = required_info(info, KEY_PROFILE)?;
        if !valid_profile(profile) {
            return Err(-libc::EINVAL);
        }

        let block_rows = optional_info(info, KEY_BLOCK_ROWS)?
            .map(parse_positive_usize)
            .transpose()?
            .unwrap_or(height as usize);
        if block_rows > height as usize || !(height as usize).is_multiple_of(block_rows) {
            return Err(-libc::EINVAL);
        }
        let blocks_per_frame = height as usize / block_rows;
        let output_rate = if blocks_per_frame == 1 {
            rate
        } else {
            calculon_spa_node::Rate::new(
                rate.num
                    .checked_mul(u32::try_from(blocks_per_frame).map_err(|_| -libc::EOVERFLOW)?)
                    .ok_or(-libc::EOVERFLOW)?,
                rate.denom,
            )?
        };

        let raw_format = Format::gray16(width, height, rate)?;
        let flat_format = Format::f32_image(FLAT_CALIBRATION_V1, profile, width, height, None)?;
        let background_format =
            Format::f32_image(BACKGROUND_CALIBRATION_V1, profile, width, height, None)?;
        let calibrated_format = Format::f32_image(
            if blocks_per_frame == 1 {
                CALIBRATED_PIXELS_V1
            } else {
                CALIBRATED_PIXEL_ROW_BLOCK_V1
            },
            profile,
            width,
            u32::try_from(block_rows).map_err(|_| -libc::EOVERFLOW)?,
            Some(output_rate),
        )?;
        let length = (width as usize)
            .checked_mul(height as usize)
            .ok_or(-libc::EOVERFLOW)?;
        let identity_flat: Arc<[f32]> = Arc::from(vec![1.0; length]);
        let identity_background: Arc<[f32]> = Arc::from(vec![0.0; length]);
        let plan =
            PixelCalibrationPlan::new(Arc::clone(&identity_flat), Arc::clone(&identity_background))
                .map_err(|_| -libc::EINVAL)?;

        Ok(Self {
            ports: vec![
                Port::new(
                    PortRef {
                        direction: sys::SPA_DIRECTION_INPUT,
                        id: 0,
                    },
                    true,
                    false,
                    [FormatConstraint::exact(raw_format)],
                )
                .with_latest_transport(),
                Port::new(
                    PortRef {
                        direction: sys::SPA_DIRECTION_INPUT,
                        id: 1,
                    },
                    false,
                    true,
                    [FormatConstraint::exact(flat_format)],
                ),
                Port::new(
                    PortRef {
                        direction: sys::SPA_DIRECTION_INPUT,
                        id: 2,
                    },
                    false,
                    true,
                    [FormatConstraint::exact(background_format)],
                ),
                Port::new(
                    PortRef {
                        direction: sys::SPA_DIRECTION_OUTPUT,
                        id: 0,
                    },
                    true,
                    false,
                    [FormatConstraint::exact(calibrated_format)],
                ),
            ],
            width: width as usize,
            height: height as usize,
            block_rows,
            flats: PlaneStore::new(length),
            backgrounds: PlaneStore::new(length),
            identity_flat: Arc::clone(&identity_flat),
            identity_background: Arc::clone(&identity_background),
            flat: identity_flat,
            background: identity_background,
            flat_seq: None,
            background_seq: None,
            plan,
            active_plan: None,
            workspace: ProgressWorkspace::new(),
            raw: vec![0; length].into_boxed_slice(),
            calibrated: vec![0.0; length].into_boxed_slice(),
            next_row: 0,
            active_seq: None,
            discontinuity: false,
        })
    }

    fn ports(&self) -> &[Port] {
        &self.ports
    }

    fn ports_mut(&mut self) -> &mut [Port] {
        &mut self.ports
    }

    fn enum_param(&self, id: u32, index: u32) -> Result<Option<PodValue>, i32> {
        let roles = [
            (PROP_FLAT, "flat", self.flat_seq),
            (PROP_BACKGROUND, "background", self.background_seq),
        ];
        match id {
            sys::SPA_PARAM_PropInfo => {
                let Some((key, name, seq)) = roles.get(index as usize) else {
                    return Ok(None);
                };
                Ok(Some(object(
                    sys::SPA_TYPE_OBJECT_PropInfo,
                    id,
                    vec![
                        Property::new(sys::SPA_PROP_INFO_id, PodValue::Id(Id(*key))),
                        Property::new(
                            sys::SPA_PROP_INFO_name,
                            PodValue::String(format!("calculon.pixel-calibration.activate-{name}")),
                        ),
                        Property::new(
                            sys::SPA_PROP_INFO_description,
                            PodValue::String("Prepared calibration sequence to activate".into()),
                        ),
                        Property::new(
                            sys::SPA_PROP_INFO_type,
                            PodValue::Long(seq.map_or(-1, |seq| seq as i64)),
                        ),
                    ],
                )))
            }
            sys::SPA_PARAM_Props if index == 0 => Ok(Some(object(
                sys::SPA_TYPE_OBJECT_Props,
                id,
                roles
                    .into_iter()
                    .map(|(key, _, seq)| {
                        Property::new(key, PodValue::Long(seq.map_or(-1, |seq| seq as i64)))
                    })
                    .collect(),
            ))),
            sys::SPA_PARAM_Props => Ok(None),
            _ => Err(-libc::ENOENT),
        }
    }

    fn set_param(
        &mut self,
        id: u32,
        flags: u32,
        value: Option<PodValue>,
        _started: bool,
    ) -> Result<(), i32> {
        if id != sys::SPA_PARAM_Props {
            return Err(-libc::ENOENT);
        }
        let mut flat = Arc::clone(&self.flat);
        let mut background = Arc::clone(&self.background);
        let mut flat_seq = self.flat_seq;
        let mut background_seq = self.background_seq;
        match value {
            None => {
                flat = Arc::clone(&self.identity_flat);
                background = Arc::clone(&self.identity_background);
                flat_seq = None;
                background_seq = None;
            }
            Some(value) => {
                let mut flat_seen = false;
                let mut background_seen = false;
                for property in parse_props(value)? {
                    let selection = match property.value {
                        PodValue::Long(-1) => None,
                        PodValue::Long(value) if value >= 0 => Some(value as u64),
                        _ if matches!(property.key, PROP_FLAT | PROP_BACKGROUND) => {
                            return Err(-libc::EPROTOTYPE);
                        }
                        _ => continue,
                    };
                    match property.key {
                        PROP_FLAT => {
                            if flat_seen {
                                return Err(-libc::EINVAL);
                            }
                            flat = self.selected_plane(INPUT_FLAT, selection)?;
                            flat_seq = selection;
                            flat_seen = true;
                        }
                        PROP_BACKGROUND => {
                            if background_seen {
                                return Err(-libc::EINVAL);
                            }
                            background = self.selected_plane(INPUT_BACKGROUND, selection)?;
                            background_seq = selection;
                            background_seen = true;
                        }
                        _ => continue,
                    }
                }
                if !flat_seen || !background_seen {
                    return Err(-libc::EINVAL);
                }
            }
        }
        let plan = PixelCalibrationPlan::new(Arc::clone(&flat), Arc::clone(&background))
            .map_err(|_| -libc::EINVAL)?;
        if flags & sys::SPA_NODE_PARAM_FLAG_TEST_ONLY == 0 {
            self.flat = flat;
            self.background = background;
            self.flat_seq = flat_seq;
            self.background_seq = background_seq;
            self.plan = plan;
        }
        Ok(())
    }

    fn ready(&self) -> Result<(), i32> {
        let expected = self.width * self.height;
        if self.plan.len() != expected
            || self.raw.len() != expected
            || self.calibrated.len() != expected
        {
            return Err(-libc::EIO);
        }
        if self.block_rows != self.height && !self.ports[INPUT_RAW].uses_latest_transport() {
            return Err(-libc::ENOTSUP);
        }
        Ok(())
    }

    fn pause(&mut self) {
        if let Some(plan) = self.active_plan.as_ref() {
            let _ = plan.abort(&mut self.workspace);
        }
        self.reset_progressive();
        self.discontinuity = true;
    }

    fn process(&mut self) -> Result<i32, i32> {
        self.ingest(INPUT_FLAT)?;
        self.ingest(INPUT_BACKGROUND)?;
        if self.ports[INPUT_RAW].uses_latest_transport() {
            return self.process_progressive();
        }

        let (inputs, outputs) = self.ports.split_at_mut(OUTPUT);
        let input = &mut inputs[INPUT_RAW];
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
                let (raw, source_stride) = source.u16()?;
                for row in 0..self.height {
                    let source_start = row * source_stride;
                    self.raw[row * self.width..(row + 1) * self.width]
                        .copy_from_slice(&raw[source_start..source_start + self.width]);
                }
                source.header()
            };
            self.plan
                .process(&mut self.calibrated, &mut self.workspace, &self.raw)
                .map_err(|_| -libc::EINVAL)?;
            let mut destination = OutputFrame::new(output_buffer, output_format)?;
            let (calibrated, destination_stride) = destination.f32_mut()?;
            for row in 0..self.height {
                let destination_start = row * destination_stride;
                calibrated[destination_start..destination_start + self.width]
                    .copy_from_slice(&self.calibrated[row * self.width..(row + 1) * self.width]);
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn prepared_sequence_identity_cannot_be_redefined() {
        let mut store = PlaneStore::new(4);
        assert_eq!(store.ingest(7, &[1.0, 2.0, 3.0, 4.0], 2, 2, 2), Ok(()));
        assert_eq!(store.ingest(7, &[1.0, 2.0, 3.0, 4.0], 2, 2, 2), Ok(()));
        assert_eq!(
            store.ingest(7, &[1.0, 2.0, 3.0, 5.0], 2, 2, 2),
            Err(-libc::EEXIST)
        );
    }
}
