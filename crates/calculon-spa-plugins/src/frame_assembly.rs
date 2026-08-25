//! Assembly of calibrated row-block ndarrays into complete detector frames.

use calculon_algorithms::schemas::CALIBRATED_PIXELS_V1;
use calculon_spa_node::{
    Factory, Format, FormatConstraint, Header, InputFrame, Node, OutputFrame, Port, PortRef, Rate,
    sys,
};

use crate::CALIBRATED_PIXEL_ROW_BLOCK_V1;
use crate::config::{parse_positive_usize, parse_rate, parse_size, required_info, valid_profile};

/// Factory name of the calibrated row-block frame assembler.
pub const FRAME_ASSEMBLY_FACTORY_NAME: &str = "api.calculon.frame-assembly";

const KEY_SIZE: &[u8] = b"api.calculon.detector-size\0";
const KEY_RATE: &[u8] = b"api.calculon.detector-rate\0";
const KEY_PROFILE: &[u8] = b"api.calculon.detector-profile\0";
const KEY_BLOCK_ROWS: &[u8] = b"api.calculon.row-block-rows\0";
const INPUT: usize = 0;
const OUTPUT: usize = 1;

pub(crate) static FACTORY: Factory =
    Factory::new::<FrameAssemblyNode>(b"api.calculon.frame-assembly\0");

struct FrameAssemblyNode {
    ports: Vec<Port>,
    width: usize,
    height: usize,
    block_rows: usize,
    frame: Box<[f32]>,
    active_seq: Option<u64>,
    next_row: usize,
    complete_header: Option<Header>,
    discontinuity: bool,
}

impl FrameAssemblyNode {
    fn abandon_frame(&mut self) {
        self.active_seq = None;
        self.next_row = 0;
        self.discontinuity = true;
    }

    fn publish_complete(&mut self) -> Result<i32, i32> {
        let Some(mut header) = self.complete_header else {
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        };
        let output = &mut self.ports[OUTPUT];
        if output.output_pending()? {
            return Ok(sys::SPA_STATUS_HAVE_DATA as i32);
        }
        let Some((output_id, output_buffer)) = output.reserve_output()? else {
            return Ok(sys::SPA_STATUS_HAVE_DATA as i32);
        };
        let output_format = output.format().ok_or(-libc::EIO)?;
        let result = (|| unsafe {
            let mut destination = OutputFrame::new(output_buffer, output_format)?;
            let (values, stride) = destination.f32_mut()?;
            for row in 0..self.height {
                values[row * stride..row * stride + self.width]
                    .copy_from_slice(&self.frame[row * self.width..(row + 1) * self.width]);
            }
            header.offset = 0;
            header.flags |= sys::SPA_META_HEADER_FLAG_MARKER;
            if self.discontinuity {
                header.flags |= sys::SPA_META_HEADER_FLAG_DISCONT;
            }
            destination.set_header(Some(header));
            destination.commit();
            Ok::<(), i32>(())
        })();
        if let Err(error) = result {
            output.cancel_output(output_id)?;
            return Err(error);
        }
        output.publish_output(output_id)?;
        self.complete_header = None;
        self.discontinuity = false;
        Ok(sys::SPA_STATUS_HAVE_DATA as i32)
    }
}

impl Node for FrameAssemblyNode {
    fn new(info: Option<&sys::spa_dict>) -> Result<Self, i32> {
        let (width, height) = parse_size(required_info(info, KEY_SIZE)?)?;
        let frame_rate = parse_rate(required_info(info, KEY_RATE)?)?;
        let profile = required_info(info, KEY_PROFILE)?;
        let block_rows = parse_positive_usize(required_info(info, KEY_BLOCK_ROWS)?)?;
        if !valid_profile(profile)
            || block_rows >= height as usize
            || !(height as usize).is_multiple_of(block_rows)
        {
            return Err(-libc::EINVAL);
        }
        let blocks = height as usize / block_rows;
        let block_rate = Rate::new(
            frame_rate
                .num
                .checked_mul(u32::try_from(blocks).map_err(|_| -libc::EOVERFLOW)?)
                .ok_or(-libc::EOVERFLOW)?,
            frame_rate.denom,
        )?;
        let input_format = Format::f32_image(
            CALIBRATED_PIXEL_ROW_BLOCK_V1,
            profile,
            width,
            u32::try_from(block_rows).map_err(|_| -libc::EOVERFLOW)?,
            Some(block_rate),
        )?;
        let output_format = Format::f32_image(
            CALIBRATED_PIXELS_V1,
            profile,
            width,
            height,
            Some(frame_rate),
        )?;
        let pixels = (width as usize)
            .checked_mul(height as usize)
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
            block_rows,
            frame: vec![0.0; pixels].into_boxed_slice(),
            active_seq: None,
            next_row: 0,
            complete_header: None,
            discontinuity: false,
        })
    }

    fn ports(&self) -> &[Port] {
        &self.ports
    }

    fn ports_mut(&mut self) -> &mut [Port] {
        &mut self.ports
    }

    fn pause(&mut self) {
        self.active_seq = None;
        self.next_row = 0;
        self.complete_header = None;
        self.discontinuity = true;
    }

    fn process(&mut self) -> Result<i32, i32> {
        if self.complete_header.is_some() {
            return self.publish_complete();
        }
        if self.ports[OUTPUT].output_pending()? {
            return Ok(sys::SPA_STATUS_HAVE_DATA as i32);
        }
        let Some((_id, input_buffer)) = self.ports[INPUT].input_buffer()? else {
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        };
        let Some(input_format) = self.ports[INPUT].format() else {
            self.ports[INPUT].consume_input()?;
            self.abandon_frame();
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        };
        let source = match unsafe { InputFrame::new(input_buffer, input_format) } {
            Ok(source) => source,
            Err(_) => {
                self.ports[INPUT].consume_input()?;
                self.abandon_frame();
                return Ok(sys::SPA_STATUS_NEED_DATA as i32);
            }
        };
        let Some(header) = source.header() else {
            self.ports[INPUT].consume_input()?;
            self.abandon_frame();
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        };
        let start_row = header.offset as usize;
        let Some(end_row) = start_row.checked_add(self.block_rows) else {
            self.ports[INPUT].consume_input()?;
            self.abandon_frame();
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        };
        let marker = header.flags & sys::SPA_META_HEADER_FLAG_MARKER != 0;
        if header.flags & sys::SPA_META_HEADER_FLAG_DISCONT != 0 {
            self.discontinuity = true;
        }

        if self.active_seq != Some(header.seq) {
            if self.active_seq.is_some() {
                self.abandon_frame();
            }
            if start_row != 0 {
                self.ports[INPUT].consume_input()?;
                self.abandon_frame();
                return Ok(sys::SPA_STATUS_NEED_DATA as i32);
            }
            self.active_seq = Some(header.seq);
            self.next_row = 0;
        }
        if start_row != self.next_row || end_row > self.height || marker != (end_row == self.height)
        {
            self.ports[INPUT].consume_input()?;
            self.abandon_frame();
            return Ok(sys::SPA_STATUS_NEED_DATA as i32);
        }

        let (values, stride) = match source.f32() {
            Ok(values) => values,
            Err(_) => {
                self.ports[INPUT].consume_input()?;
                self.abandon_frame();
                return Ok(sys::SPA_STATUS_NEED_DATA as i32);
            }
        };
        for row in 0..self.block_rows {
            let source_start = row * stride;
            let destination_start = (start_row + row) * self.width;
            self.frame[destination_start..destination_start + self.width]
                .copy_from_slice(&values[source_start..source_start + self.width]);
        }
        self.ports[INPUT].consume_input()?;
        self.next_row = end_row;
        if marker {
            self.complete_header = Some(header);
            self.active_seq = None;
            self.next_row = 0;
            return self.publish_complete();
        }
        Ok(sys::SPA_STATUS_NEED_DATA as i32)
    }
}
