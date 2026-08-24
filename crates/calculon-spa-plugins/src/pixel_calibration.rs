//! Complete-frame detector calibration with independently prepared planes.

use std::sync::Arc;

use calculon_algorithms::schemas::{
    BACKGROUND_CALIBRATION_V1, CALIBRATED_PIXELS_V1, FLAT_CALIBRATION_V1,
};
use calculon_algorithms::{AlgorithmPlan, PixelCalibrationPlan, ProgressWorkspace};
use calculon_spa_node::{
    Factory, Format, FormatConstraint, InputFrame, Node, OutputFrame, PodValue, Port, PortRef,
    Property, object, parse_props, sys,
};
use libspa::utils::Id;

use crate::config::{parse_rate, parse_size, required_info, valid_profile};

/// Factory name of the complete-frame detector-calibration node.
pub const PIXEL_CALIBRATION_FACTORY_NAME: &str = "api.calculon.pixel-calibration";

const KEY_SIZE: &[u8] = b"api.calculon.detector-size\0";
const KEY_RATE: &[u8] = b"api.calculon.detector-rate\0";
const KEY_PROFILE: &[u8] = b"api.calculon.detector-profile\0";
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
    flats: PlaneStore,
    backgrounds: PlaneStore,
    identity_flat: Arc<[f32]>,
    identity_background: Arc<[f32]>,
    flat: Arc<[f32]>,
    background: Arc<[f32]>,
    flat_seq: Option<u64>,
    background_seq: Option<u64>,
    plan: PixelCalibrationPlan<f32, u16>,
    workspace: ProgressWorkspace,
    raw: Box<[u16]>,
    calibrated: Box<[f32]>,
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

        let raw_format = Format::gray16(width, height, rate)?;
        let flat_format = Format::f32_image(FLAT_CALIBRATION_V1, profile, width, height, None)?;
        let background_format =
            Format::f32_image(BACKGROUND_CALIBRATION_V1, profile, width, height, None)?;
        let calibrated_format =
            Format::f32_image(CALIBRATED_PIXELS_V1, profile, width, height, Some(rate))?;
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
                ),
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
            flats: PlaneStore::new(length),
            backgrounds: PlaneStore::new(length),
            identity_flat: Arc::clone(&identity_flat),
            identity_background: Arc::clone(&identity_background),
            flat: identity_flat,
            background: identity_background,
            flat_seq: None,
            background_seq: None,
            plan,
            workspace: ProgressWorkspace::new(),
            raw: vec![0; length].into_boxed_slice(),
            calibrated: vec![0.0; length].into_boxed_slice(),
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
        Ok(())
    }

    fn process(&mut self) -> Result<i32, i32> {
        self.ingest(INPUT_FLAT)?;
        self.ingest(INPUT_BACKGROUND)?;

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
