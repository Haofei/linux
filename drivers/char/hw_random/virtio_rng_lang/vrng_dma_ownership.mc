// SPDX-License-Identifier: GPL-2.0-or-later

// M6 qualification variant. Linux still allocates and maps the DMA storage.
// This narrow adoption boundary gives MC one linear CPU-owned handle; handing
// it to the device consumes that handle and removes every CPU access API until
// the completion path consumes the device-owned handle during reclaim.

move struct VrngCpuOwnedBuffer {
    cpu: [*]mut u8,
    len: u32,
}

move struct VrngDeviceOwnedBuffer {
    dma: DmaAddr,
    len: u32,
}

// Audited C/Linux boundary. The caller guarantees that `cpu` names `len`
// mapped bytes and that no external C alias accesses them while device-owned.
extern fn vrng_dma_clean_for_device_base(cpu: [*]mut u8, len: u32) -> usize;
extern fn vrng_dma_invalidate_for_cpu_base(dma: DmaAddr, len: u32) -> [*]mut u8;

fn adopt_linux_buffer(cpu: [*]mut u8, len: u32) -> VrngCpuOwnedBuffer {
    return .{ .cpu = cpu, .len = len };
}

fn hand_to_device(buffer: VrngCpuOwnedBuffer) -> VrngDeviceOwnedBuffer {
    let cpu: [*]mut u8 = buffer.cpu;
    let len: u32 = buffer.len;
    let raw_dma: usize = vrng_dma_clean_for_device_base(cpu, len);
    var dma: DmaAddr = uninit;
    unsafe {
        dma = raw_dma as DmaAddr;
        forget_unchecked(buffer);
    }
    return .{ .dma = dma, .len = len };
}

fn reclaim_from_device(buffer: VrngDeviceOwnedBuffer) -> VrngCpuOwnedBuffer {
    let dma: DmaAddr = buffer.dma;
    let len: u32 = buffer.len;
    let cpu: [*]mut u8 = vrng_dma_invalidate_for_cpu_base(dma, len);
    unsafe { forget_unchecked(buffer); }
    return .{ .cpu = cpu, .len = len };
}

fn cpu_read(buffer: *VrngCpuOwnedBuffer, offset: u32) -> u8 {
    if offset >= buffer.len {
        unreachable;
    }
    unsafe { return buffer.cpu.offset(offset as usize).*; }
}

fn release_adopted(buffer: VrngCpuOwnedBuffer) -> void {
    // Linux retains allocation ownership; MC relinquishes only its adopted
    // linear capability after the descriptor lifecycle is complete.
    unsafe { forget_unchecked(buffer); }
}

// A single function is enough to qualify the complete ownership transition in
// the compiler. The live driver remains a separate common-C-owned DMA variant.
fn qualify_dma_ownership_cycle(cpu: [*]mut u8, len: u32) -> u8 {
    let adopted: VrngCpuOwnedBuffer = adopt_linux_buffer(cpu, len);
    let device: VrngDeviceOwnedBuffer = hand_to_device(adopted);
    let reclaimed: VrngCpuOwnedBuffer = reclaim_from_device(device);
    let value: u8 = cpu_read(&reclaimed, 0);
    release_adopted(reclaimed);
    return value;
}
