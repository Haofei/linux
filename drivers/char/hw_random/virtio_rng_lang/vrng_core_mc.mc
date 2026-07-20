// SPDX-License-Identifier: GPL-2.0-or-later

// MC candidate for the virtio-rng logical buffer protocol. The externally
// visible object remains C-layout state; decode() maps it to a closed semantic
// state before any transition is performed.

const ABI_VERSION: u32 = 1;
const ACTIVE: u32 = 0;
const QUIESCING: u32 = 1;
const DEAD: u32 = 2;
const EMPTY: u32 = 0;
const DEVICE_OWNED: u32 = 1;
const READY: u32 = 2;

const EINVAL: i32 = - 22;
const ENODEV: i32 = - 19;
const EAGAIN: i32 = - 11;
const EBUSY: i32 = - 16;
const EOVERFLOW: i32 = - 75;
const ENODATA: i32 = - 61;
const EALREADY: i32 = - 114;
const ESTALE: i32 = - 116;
const U64_MAX: u64 = 18446744073709551615;
const STATE_ACTIVE_EMPTY: u32 = 0;
const STATE_ACTIVE_DEVICE_OWNED: u32 = 1;
const STATE_ACTIVE_READY: u32 = 2;
const STATE_QUIESCING_EMPTY: u32 = 3;
const STATE_QUIESCING_DEVICE_OWNED: u32 = 4;
const STATE_DEAD: u32 = 5;
const STATE_INVALID: u32 = 6;

extern struct VrngCoreState {
    abi_version: u32,
    lifecycle: u32,
    phase: u32,
    capacity: u32,
    data_idx: u32,
    data_avail: u32,
    epoch: u64,
    generation: u64,
}

open enum DecodedState: u32 {
    ActiveEmpty = 0,
    ActiveDeviceOwned = 1,
    ActiveReady = 2,
    QuiescingEmpty = 3,
    QuiescingDeviceOwned = 4,
    Dead = 5,
    Invalid = 6,
}

#[irq_context]
fn decode_code(state: *const VrngCoreState) -> u32 {
    if state.abi_version != ABI_VERSION || state.lifecycle > DEAD || state.phase > READY {
        return STATE_INVALID;
    }
    if state.capacity == 0 || state.data_idx > state.capacity {
        return STATE_INVALID;
    }
    var remaining: u32 = 0;
    #[unsafe_contract(no_overflow)]
    {
        remaining = unchecked.sub(state.capacity, state.data_idx);
    }
    if state.data_avail > remaining {
        return STATE_INVALID;
    }

    if state.lifecycle == ACTIVE {
        if state.phase == EMPTY && state.data_idx == 0 && state.data_avail == 0 {
            return STATE_ACTIVE_EMPTY;
        }
        if state.phase == DEVICE_OWNED && state.data_idx == 0 && state.data_avail == 0 {
            return STATE_ACTIVE_DEVICE_OWNED;
        }
        if state.phase == READY && state.data_avail != 0 {
            return STATE_ACTIVE_READY;
        }
        return STATE_INVALID;
    }
    if state.lifecycle == QUIESCING {
        if state.phase == EMPTY && state.data_idx == 0 && state.data_avail == 0 {
            return STATE_QUIESCING_EMPTY;
        }
        if state.phase == DEVICE_OWNED && state.data_idx == 0 && state.data_avail == 0 {
            return STATE_QUIESCING_DEVICE_OWNED;
        }
        return STATE_INVALID;
    }
    if state.phase == EMPTY && state.data_idx == 0 && state.data_avail == 0 {
        return STATE_DEAD;
    }
    return STATE_INVALID;
}

fn decode(state: *const VrngCoreState) -> DecodedState {
    switch decode_code(state) {
        0 => { return.ActiveEmpty; }
        1 => { return.ActiveDeviceOwned; }
        2 => { return.ActiveReady; }
        3 => { return.QuiescingEmpty; }
        4 => { return.QuiescingDeviceOwned; }
        5 => { return.Dead; }
        _ => { return.Invalid; }
    }
}

#[irq_context]
fn make_empty(state: *mut VrngCoreState) -> void {
    state.phase = EMPTY;
    state.data_idx = 0;
    state.data_avail = 0;
}

export fn vrng_core_mc_init(maybe_state: ?*mut VrngCoreState, capacity: u32, epoch: u64) -> i32 {
    if capacity == 0 {
        return EINVAL;
    }
    if let state = maybe_state {
        state.abi_version = ABI_VERSION;
        state.lifecycle = ACTIVE;
        state.phase = EMPTY;
        state.capacity = capacity;
        state.data_idx = 0;
        state.data_avail = 0;
        state.epoch = epoch;
        state.generation = epoch;
        return 0;
    }
    return EINVAL;
}

export fn vrng_core_mc_validate(maybe_state: ?*const VrngCoreState) -> i32 {
    if let state = maybe_state {
        if decode(state) ==.Invalid {
            return EINVAL;
        }
        return 0;
    }
    return EINVAL;
}

export fn vrng_core_mc_begin_submit(maybe_state: ?*mut VrngCoreState, maybe_generation: ?*mut u64) -> i32 {
    if let generation = maybe_generation {
        generation.* = 0;
        if let state = maybe_state {
            switch decode(state) {
                .Invalid => { return EINVAL; }
                .ActiveEmpty => {}
                .ActiveDeviceOwned,.ActiveReady => { return EBUSY; }
                _ => { return ENODEV; }
            }
            if state.generation == U64_MAX {
                return EOVERFLOW;
            }
            let next: u64 = state.generation + 1;
            state.generation = next;
            state.phase = DEVICE_OWNED;
            generation.* = next;
            return 0;
        }
    }
    return EINVAL;
}

export fn vrng_core_mc_abort_submit(maybe_state: ?*mut VrngCoreState, generation: u64) -> i32 {
    if let state = maybe_state {
        let decoded: DecodedState = decode(state);
        if decoded ==.Invalid { return EINVAL; }
        if decoded ==.Dead { return ENODEV; }
        if decoded ==.ActiveDeviceOwned || decoded ==.QuiescingDeviceOwned {
            if generation != state.generation { return ESTALE; }
            make_empty(state);
            return 0;
        }
        if generation == state.generation { return EALREADY; }
        return ESTALE;
    }
    return EINVAL;
}

#[irq_context]
export fn vrng_core_mc_complete(
    maybe_state: ?*mut VrngCoreState,
    generation: u64,
    produced: u32,
    maybe_need_resubmit: ?*mut u32,
) -> i32 {
    if let need_resubmit = maybe_need_resubmit {
        need_resubmit.* = 0;
        if let state = maybe_state {
            let decoded: u32 = decode_code(state);
            if decoded == STATE_INVALID { return EINVAL; }
            if decoded == STATE_DEAD { return ENODEV; }
            if decoded != STATE_ACTIVE_DEVICE_OWNED && decoded != STATE_QUIESCING_DEVICE_OWNED {
                if generation == state.generation { return EALREADY; }
                return ESTALE;
            }
            if generation != state.generation { return ESTALE; }
            if decoded == STATE_QUIESCING_DEVICE_OWNED {
                make_empty(state);
                return ENODEV;
            }
            if produced == 0 {
                make_empty(state);
                need_resubmit.* = 1;
                return ENODATA;
            }
            if produced > state.capacity {
                make_empty(state);
                need_resubmit.* = 1;
                return EOVERFLOW;
            }
            state.phase = READY;
            state.data_idx = 0;
            state.data_avail = produced;
            return 0;
        }
    }
    return EINVAL;
}

export fn vrng_core_mc_copy(
    maybe_state: ?*mut VrngCoreState,
    maybe_dma_buffer: ?[*]const u8,
    maybe_destination: ?[*]mut u8,
    requested: u32,
    maybe_copied: ?*mut u32,
    maybe_need_resubmit: ?*mut u32,
) -> i32 {
    if let copied = maybe_copied {
        copied.* = 0;
        if let need_resubmit = maybe_need_resubmit {
            need_resubmit.* = 0;
            if let state = maybe_state {
                let decoded: DecodedState = decode(state);
                if decoded ==.Invalid { return EINVAL; }
                if decoded ==.ActiveEmpty { return EAGAIN; }
                if decoded ==.ActiveDeviceOwned { return EBUSY; }
                if decoded !=.ActiveReady { return ENODEV; }
                if let dma_buffer = maybe_dma_buffer {
                    if let destination = maybe_destination {
                        if requested == 0 { return 0; }
                        var amount: u32 = requested;
                        if state.data_avail < amount { amount = state.data_avail; }
                        if state.data_idx > state.capacity { return EOVERFLOW; }
                        if amount > state.capacity - state.data_idx { return EOVERFLOW; }
                        var next_idx: u32 = 0;
                        var next_avail: u32 = 0;
                        // decode() and the guards prove both transition calculations.
                        #[unsafe_contract(no_overflow)]
                        {
                            next_idx = unchecked.add(state.data_idx, amount);
                            next_avail = unchecked.sub(state.data_avail, amount);
                        }
                        var i: u32 = 0;
                        while i < amount {
                            var source_index: u32 = 0;
                            // The guards above establish data_idx + amount <= capacity;
                            // i < amount proves both additions cannot overflow.
                            #[unsafe_contract(no_overflow)]
                            {
                                source_index = unchecked.add(state.data_idx, i);
                            }
                            unsafe {
                                destination.offset(i as usize).* = dma_buffer.offset(source_index as usize).*;
                            }
                            #[unsafe_contract(no_overflow)]
                            {
                                i = unchecked.add(i, 1);
                            }
                        }
                        copied.* = amount;
                        if next_avail == 0 {
                            make_empty(state);
                            need_resubmit.* = 1;
                        } else {
                            state.data_idx = next_idx;
                            state.data_avail = next_avail;
                        }
                        return 0;
                    }
                }
            }
        }
    }
    return EINVAL;
}

export fn vrng_core_mc_begin_remove(maybe_state: ?*mut VrngCoreState) -> i32 {
    if let state = maybe_state {
        let decoded: DecodedState = decode(state);
        if decoded ==.Invalid { return EINVAL; }
        if decoded ==.ActiveEmpty || decoded ==.ActiveDeviceOwned {
            state.lifecycle = QUIESCING;
            return 0;
        }
        if decoded ==.ActiveReady {
            make_empty(state);
            state.lifecycle = QUIESCING;
            return 0;
        }
        return EALREADY;
    }
    return EINVAL;
}

export fn vrng_core_mc_finish_remove(maybe_state: ?*mut VrngCoreState) -> i32 {
    if let state = maybe_state {
        let decoded: DecodedState = decode(state);
        if decoded ==.Invalid { return EINVAL; }
        if decoded ==.Dead { return EALREADY; }
        if decoded ==.QuiescingEmpty || decoded ==.QuiescingDeviceOwned {
            make_empty(state);
            state.lifecycle = DEAD;
            return 0;
        }
        return EINVAL;
    }
    return EINVAL;
}
