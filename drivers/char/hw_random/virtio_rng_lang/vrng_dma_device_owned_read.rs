// SPDX-License-Identifier: GPL-2.0-or-later

#[path = "vrng_dma_ownership.rs"]
mod ownership;

use ownership::{CpuOwned, DmaBuffer};

fn reject_device_owned_read(cpu: *mut u8, dma: usize, len: usize) -> u8 {
    let buffer = unsafe { DmaBuffer::<CpuOwned>::adopt(cpu, dma, len) };
    let mut device = buffer.handoff();
    // This method exists only for DmaBuffer<CpuOwned>.
    device.as_mut_slice()[0]
}
