// SPDX-License-Identifier: GPL-2.0-or-later

// M6 negative fixture: a device-owned handle cannot be passed to the only CPU
// read API. Qualification requires this file to fail during semantic checking.

move struct VrngCpuOwnedBuffer {
    cpu: [*]mut u8,
    len: u32,
}

move struct VrngDeviceOwnedBuffer {
    dma: DmaAddr,
    len: u32,
}

fn cpu_read(buffer: *VrngCpuOwnedBuffer, offset: u32) -> u8 {
    unsafe { return buffer.cpu.offset(offset as usize).*; }
}

fn reject_device_owned_read(buffer: *VrngDeviceOwnedBuffer) -> u8 {
    // EXPECT_ERROR: E_NO_IMPLICIT_POINTER_CONVERSION
    return cpu_read(buffer, 0);
}
