// SPDX-License-Identifier: GPL-2.0-or-later

//! M6 Rust-safe typestate peer for the MC qualification fixture.
//!
//! Linux still owns allocation and mapping.  The unsafe adoption/reclaim calls
//! are the audited boundary; safe Rust exposes CPU access only while the handle
//! is in `CpuOwned` state.  Any raw alias retained by C remains trusted.

use core::marker::PhantomData;
use core::slice;

pub enum CpuOwned {}
pub enum DeviceOwned {}

pub struct DmaBuffer<State> {
    cpu: *mut u8,
    dma: usize,
    len: usize,
    _state: PhantomData<State>,
}

impl DmaBuffer<CpuOwned> {
    /// # Safety
    ///
    /// `cpu..cpu+len` must be a live uniquely CPU-accessible mapping.  External
    /// aliases must obey the same handoff/reclaim protocol.
    pub unsafe fn adopt(cpu: *mut u8, dma: usize, len: usize) -> Self {
        Self {
            cpu,
            dma,
            len,
            _state: PhantomData,
        }
    }

    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        // SAFETY: established by adopt/reclaim and unavailable in DeviceOwned.
        unsafe { slice::from_raw_parts_mut(self.cpu, self.len) }
    }

    pub fn handoff(self) -> DmaBuffer<DeviceOwned> {
        DmaBuffer {
            cpu: self.cpu,
            dma: self.dma,
            len: self.len,
            _state: PhantomData,
        }
    }
}

impl DmaBuffer<DeviceOwned> {
    pub fn dma_addr(&self) -> usize {
        self.dma
    }

    /// # Safety
    ///
    /// The device must be drained and DMA synchronization for CPU complete.
    pub unsafe fn reclaim(self) -> DmaBuffer<CpuOwned> {
        DmaBuffer {
            cpu: self.cpu,
            dma: self.dma,
            len: self.len,
            _state: PhantomData,
        }
    }
}

pub unsafe fn qualify_dma_ownership_cycle(cpu: *mut u8, dma: usize, len: usize) -> u8 {
    let buffer = unsafe { DmaBuffer::<CpuOwned>::adopt(cpu, dma, len) };
    let device = buffer.handoff();
    let _published_dma = device.dma_addr();
    let mut reclaimed = unsafe { device.reclaim() };
    reclaimed.as_mut_slice()[0]
}
