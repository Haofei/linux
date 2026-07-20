// SPDX-License-Identifier: GPL-2.0-or-later

//! Rust candidate for the virtio-rng logical buffer protocol.

use core::{cmp, ptr};
use kernel::bindings;

const ABI_VERSION: u32 = 1;
const ACTIVE: u32 = 0;
const QUIESCING: u32 = 1;
const DEAD: u32 = 2;
const EMPTY: u32 = 0;
const DEVICE_OWNED: u32 = 1;
const READY: u32 = 2;

const fn errno(value: u32) -> i32 {
    -(value as i32)
}

#[repr(C, align(8))]
struct VrngCoreState {
    abi_version: u32,
    lifecycle: u32,
    phase: u32,
    capacity: u32,
    data_idx: u32,
    data_avail: u32,
    epoch: u64,
    generation: u64,
}

enum DecodedState {
    ActiveEmpty,
    ActiveDeviceOwned,
    ActiveReady { index: u32, available: u32 },
    QuiescingEmpty,
    QuiescingDeviceOwned,
    Dead,
}

extern "C" {
    fn vrng_core_index_nospec(index: u32, size: u32) -> u32;
}

fn decode(state: &VrngCoreState) -> core::result::Result<DecodedState, i32> {
    if state.abi_version != ABI_VERSION
        || state.lifecycle > DEAD
        || state.phase > READY
        || state.capacity == 0
        || state.data_idx > state.capacity
        || state.data_avail > state.capacity
    {
        return Err(errno(bindings::EINVAL));
    }
    let Some(end) = state.data_idx.checked_add(state.data_avail) else {
        return Err(errno(bindings::EINVAL));
    };
    if end > state.capacity {
        return Err(errno(bindings::EINVAL));
    }

    match (state.lifecycle, state.phase) {
        (ACTIVE, EMPTY) if state.data_idx == 0 && state.data_avail == 0 => {
            Ok(DecodedState::ActiveEmpty)
        }
        (ACTIVE, DEVICE_OWNED) if state.data_idx == 0 && state.data_avail == 0 => {
            Ok(DecodedState::ActiveDeviceOwned)
        }
        (ACTIVE, READY) if state.data_avail != 0 => Ok(DecodedState::ActiveReady {
            index: state.data_idx,
            available: state.data_avail,
        }),
        (QUIESCING, EMPTY) if state.data_idx == 0 && state.data_avail == 0 => {
            Ok(DecodedState::QuiescingEmpty)
        }
        (QUIESCING, DEVICE_OWNED) if state.data_idx == 0 && state.data_avail == 0 => {
            Ok(DecodedState::QuiescingDeviceOwned)
        }
        (DEAD, EMPTY) if state.data_idx == 0 && state.data_avail == 0 => Ok(DecodedState::Dead),
        _ => Err(errno(bindings::EINVAL)),
    }
}

fn make_empty(state: &mut VrngCoreState) {
    state.phase = EMPTY;
    state.data_idx = 0;
    state.data_avail = 0;
}

#[no_mangle]
unsafe extern "C" fn vrng_core_rust_init(
    state: *mut VrngCoreState,
    capacity: u32,
    epoch: u64,
) -> i32 {
    if state.is_null() || capacity == 0 {
        return errno(bindings::EINVAL);
    }
    // SAFETY: The ABI requires a valid, aligned, exclusively owned state pointer.
    unsafe {
        state.write(VrngCoreState {
            abi_version: ABI_VERSION,
            lifecycle: ACTIVE,
            phase: EMPTY,
            capacity,
            data_idx: 0,
            data_avail: 0,
            epoch,
            generation: epoch,
        });
    }
    0
}

#[no_mangle]
unsafe extern "C" fn vrng_core_rust_begin_submit(
    state: *mut VrngCoreState,
    generation: *mut u64,
) -> i32 {
    if generation.is_null() {
        return errno(bindings::EINVAL);
    }
    // SAFETY: The ABI requires a valid writable output pointer.
    unsafe { generation.write(0) };
    // SAFETY: The glue serializes calls and guarantees exclusive access.
    let Some(state) = (unsafe { state.as_mut() }) else {
        return errno(bindings::EINVAL);
    };
    match decode(state) {
        Ok(DecodedState::ActiveEmpty) => {}
        Ok(DecodedState::ActiveDeviceOwned | DecodedState::ActiveReady { .. }) => {
            return errno(bindings::EBUSY);
        }
        Ok(_) => return errno(bindings::ENODEV),
        Err(error) => return error,
    }
    let Some(next) = state.generation.checked_add(1) else {
        return errno(bindings::EOVERFLOW);
    };
    state.generation = next;
    state.phase = DEVICE_OWNED;
    // SAFETY: The output pointer was validated above and does not alias state.
    unsafe { generation.write(next) };
    0
}

#[no_mangle]
unsafe extern "C" fn vrng_core_rust_abort_submit(
    state: *mut VrngCoreState,
    generation: u64,
) -> i32 {
    // SAFETY: The glue serializes calls and guarantees exclusive access.
    let Some(state) = (unsafe { state.as_mut() }) else {
        return errno(bindings::EINVAL);
    };
    match decode(state) {
        Err(error) => error,
        Ok(DecodedState::Dead) => errno(bindings::ENODEV),
        Ok(DecodedState::ActiveDeviceOwned | DecodedState::QuiescingDeviceOwned) => {
            if generation != state.generation {
                errno(bindings::ESTALE)
            } else {
                make_empty(state);
                0
            }
        }
        Ok(_) if generation == state.generation => errno(bindings::EALREADY),
        Ok(_) => errno(bindings::ESTALE),
    }
}

#[no_mangle]
unsafe extern "C" fn vrng_core_rust_complete(
    state: *mut VrngCoreState,
    generation: u64,
    produced: u32,
    need_resubmit: *mut u32,
) -> i32 {
    if need_resubmit.is_null() {
        return errno(bindings::EINVAL);
    }
    // SAFETY: The ABI requires a valid writable output pointer.
    unsafe { need_resubmit.write(0) };
    // SAFETY: The glue serializes calls and guarantees exclusive access.
    let Some(state) = (unsafe { state.as_mut() }) else {
        return errno(bindings::EINVAL);
    };
    let decoded = match decode(state) {
        Ok(decoded) => decoded,
        Err(error) => return error,
    };
    match decoded {
        DecodedState::Dead => return errno(bindings::ENODEV),
        DecodedState::ActiveDeviceOwned | DecodedState::QuiescingDeviceOwned => {}
        _ if generation == state.generation => return errno(bindings::EALREADY),
        _ => return errno(bindings::ESTALE),
    }
    if generation != state.generation {
        return errno(bindings::ESTALE);
    }
    if matches!(decoded, DecodedState::QuiescingDeviceOwned) {
        make_empty(state);
        return errno(bindings::ENODEV);
    }
    if produced == 0 {
        make_empty(state);
        // SAFETY: The output pointer was validated and does not alias state.
        unsafe { need_resubmit.write(1) };
        return errno(bindings::ENODATA);
    }
    if produced > state.capacity {
        make_empty(state);
        // SAFETY: The output pointer was validated and does not alias state.
        unsafe { need_resubmit.write(1) };
        return errno(bindings::EOVERFLOW);
    }
    state.phase = READY;
    state.data_idx = 0;
    state.data_avail = produced;
    0
}

#[no_mangle]
unsafe extern "C" fn vrng_core_rust_copy(
    state: *mut VrngCoreState,
    dma_buffer: *const u8,
    destination: *mut u8,
    requested: u32,
    copied: *mut u32,
    need_resubmit: *mut u32,
) -> i32 {
    if copied.is_null() || need_resubmit.is_null() {
        return errno(bindings::EINVAL);
    }
    // SAFETY: The ABI requires valid writable output pointers.
    unsafe {
        copied.write(0);
        need_resubmit.write(0);
    }
    // SAFETY: The glue serializes calls and guarantees exclusive access.
    let Some(state) = (unsafe { state.as_mut() }) else {
        return errno(bindings::EINVAL);
    };
    let (index, available) = match decode(state) {
        Err(error) => return error,
        Ok(DecodedState::ActiveEmpty) => return errno(bindings::EAGAIN),
        Ok(DecodedState::ActiveDeviceOwned) => return errno(bindings::EBUSY),
        Ok(DecodedState::ActiveReady { index, available }) => (index, available),
        Ok(_) => return errno(bindings::ENODEV),
    };
    if dma_buffer.is_null() || destination.is_null() {
        return errno(bindings::EINVAL);
    }
    if requested == 0 {
        return 0;
    }

    let amount = cmp::min(requested, available);
    // SAFETY: The common C boundary wraps Linux's array_index_nospec().
    // Decoding proved that a ready state's index is strictly below capacity.
    let hardened_index = unsafe { vrng_core_index_nospec(index, state.capacity) };
    let Some(next_index) = hardened_index.checked_add(amount) else {
        return errno(bindings::EOVERFLOW);
    };
    let Some(next_available) = available.checked_sub(amount) else {
        return errno(bindings::EOVERFLOW);
    };
    if next_index > state.capacity {
        return errno(bindings::EOVERFLOW);
    }

    // SAFETY: Decoding proved hardened_index + amount <= capacity. The ABI requires
    // dma_buffer to cover capacity bytes, destination to cover requested bytes,
    // and the two ranges not to overlap.
    unsafe {
        ptr::copy_nonoverlapping(
            dma_buffer.add(hardened_index as usize),
            destination,
            amount as usize,
        );
        copied.write(amount);
    }
    if next_available == 0 {
        make_empty(state);
        // SAFETY: The output pointer was validated and does not alias state.
        unsafe { need_resubmit.write(1) };
    } else {
        state.data_idx = next_index;
        state.data_avail = next_available;
    }
    0
}

#[no_mangle]
unsafe extern "C" fn vrng_core_rust_begin_remove(state: *mut VrngCoreState) -> i32 {
    // SAFETY: The glue serializes calls and guarantees exclusive access.
    let Some(state) = (unsafe { state.as_mut() }) else {
        return errno(bindings::EINVAL);
    };
    match decode(state) {
        Err(error) => error,
        Ok(DecodedState::ActiveEmpty | DecodedState::ActiveDeviceOwned) => {
            state.lifecycle = QUIESCING;
            0
        }
        Ok(DecodedState::ActiveReady { .. }) => {
            make_empty(state);
            state.lifecycle = QUIESCING;
            0
        }
        Ok(_) => errno(bindings::EALREADY),
    }
}

#[no_mangle]
unsafe extern "C" fn vrng_core_rust_finish_remove(state: *mut VrngCoreState) -> i32 {
    // SAFETY: The glue serializes calls and guarantees exclusive access.
    let Some(state) = (unsafe { state.as_mut() }) else {
        return errno(bindings::EINVAL);
    };
    match decode(state) {
        Err(error) => error,
        Ok(DecodedState::Dead) => errno(bindings::EALREADY),
        Ok(DecodedState::QuiescingEmpty | DecodedState::QuiescingDeviceOwned) => {
            make_empty(state);
            state.lifecycle = DEAD;
            0
        }
        Ok(_) => errno(bindings::EINVAL),
    }
}

#[no_mangle]
unsafe extern "C" fn vrng_core_rust_validate(state: *const VrngCoreState) -> i32 {
    // SAFETY: The ABI requires a valid, aligned state pointer for the call.
    let Some(state) = (unsafe { state.as_ref() }) else {
        return errno(bindings::EINVAL);
    };
    match decode(state) {
        Ok(_) => 0,
        Err(error) => error,
    }
}
