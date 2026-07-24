// SPDX-License-Identifier: GPL-2.0-or-later

//! Raw-FFI and safe-value Rust candidates for the common driver lifecycle.

use core::ptr;
use kernel::bindings;

const ABI_VERSION: u32 = 1;
const RESET: u32 = 0;
const LIVE: u32 = 1;
const REMOVING: u32 = 2;
const DRAINED: u32 = 3;
const DEAD: u32 = 4;

const INIT: u32 = 1;
const REGISTER: u32 = 2;
const CALLBACK_COMPLETE: u32 = 3;
const PUBLISH: u32 = 4;
const BEGIN_REMOVE: u32 = 5;
const DRAIN: u32 = 6;
const FINAL_CLEAR: u32 = 7;
const FINISH_REMOVE: u32 = 8;

const fn errno(value: u32) -> i32 {
    -(value as i32)
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DriverState {
    abi_version: u32,
    stage: u32,
    registered: u32,
    callback_drained: u32,
    publication_pending: u32,
    pending_len: u32,
    external_avail: u32,
    reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DriverOutcome {
    unregister_required: u32,
    reserved: u32,
}

const EMPTY_OUTCOME: DriverOutcome = DriverOutcome {
    unregister_required: 0,
    reserved: 0,
};

fn raw_valid(state: &DriverState) -> bool {
    state.abi_version == ABI_VERSION
        && state.stage <= DEAD
        && state.registered <= 1
        && state.callback_drained <= 1
        && state.publication_pending <= 1
        && state.publication_pending == u32::from(state.pending_len != 0)
        && !(state.stage == LIVE && state.callback_drained != 0)
        && !(state.stage >= DRAINED
            && (state.callback_drained == 0
                || state.registered != 0
                || state.publication_pending != 0))
        && !(state.stage == DEAD && state.external_avail != 0)
}

fn raw_transition(
    state: &mut DriverState,
    event: u32,
    value: u32,
    outcome: &mut DriverOutcome,
) -> i32 {
    match event {
        REGISTER => {
            if state.stage != LIVE {
                return errno(bindings::ENODEV);
            }
            if state.registered != 0 {
                return errno(bindings::EALREADY);
            }
            state.registered = u32::from(value != 0);
        }
        CALLBACK_COMPLETE => {
            if (state.stage != LIVE && state.stage != REMOVING)
                || state.callback_drained != 0
                || value == 0
            {
                return errno(bindings::EINVAL);
            }
            state.publication_pending = 1;
            state.pending_len = value;
        }
        PUBLISH => {
            if state.publication_pending == 0 || state.callback_drained != 0 {
                return errno(bindings::EINVAL);
            }
            state.external_avail = state.pending_len;
            state.publication_pending = 0;
            state.pending_len = 0;
        }
        BEGIN_REMOVE => {
            if state.stage != LIVE {
                return errno(bindings::EALREADY);
            }
            state.stage = REMOVING;
            outcome.unregister_required = state.registered;
            state.registered = 0;
        }
        DRAIN => {
            if state.stage != REMOVING {
                return errno(bindings::EINVAL);
            }
            state.stage = DRAINED;
            state.callback_drained = 1;
            state.publication_pending = 0;
            state.pending_len = 0;
        }
        FINAL_CLEAR => {
            if state.stage != DRAINED {
                return errno(bindings::EBUSY);
            }
            state.external_avail = 0;
        }
        FINISH_REMOVE => {
            if state.stage != DRAINED
                || state.external_avail != 0
                || state.publication_pending != 0
            {
                return errno(bindings::EBUSY);
            }
            state.stage = DEAD;
        }
        _ => return errno(bindings::EINVAL),
    }
    0
}

unsafe fn raw_entry(
    state: *mut DriverState,
    event: u32,
    value: u32,
    outcome: *mut DriverOutcome,
) -> i32 {
    if !outcome.is_null() {
        // SAFETY: The ABI requires a writable outcome object.
        unsafe { outcome.write(EMPTY_OUTCOME) };
    }
    let Some(outcome) = (unsafe { outcome.as_mut() }) else {
        return errno(bindings::EINVAL);
    };
    let Some(state) = (unsafe { state.as_mut() }) else {
        return errno(bindings::EINVAL);
    };
    if event == INIT {
        *state = DriverState {
            abi_version: ABI_VERSION,
            stage: LIVE,
            registered: 0,
            callback_drained: 0,
            publication_pending: 0,
            pending_len: 0,
            external_avail: 0,
            reserved: 0,
        };
        return 0;
    }
    if !raw_valid(state) {
        return errno(bindings::EINVAL);
    }
    raw_transition(state, event, value, outcome)
}

#[no_mangle]
unsafe extern "C" fn vrng_driver_rust_raw_step(
    state: *mut DriverState,
    event: u32,
    value: u32,
    outcome: *mut DriverOutcome,
) -> i32 {
    // SAFETY: This is the declared raw-FFI comparison boundary.
    unsafe { raw_entry(state, event, value, outcome) }
}

#[derive(Clone, Copy, PartialEq)]
enum Stage {
    Live,
    Removing,
    Drained,
    Dead,
}

struct SafeState {
    stage: Stage,
    registered: bool,
    callback_drained: bool,
    pending_len: Option<u32>,
    external_avail: u32,
}

fn decode_safe(raw: DriverState) -> Result<SafeState, i32> {
    if !raw_valid(&raw) || raw.stage == RESET {
        return Err(errno(bindings::EINVAL));
    }
    let stage = match raw.stage {
        LIVE => Stage::Live,
        REMOVING => Stage::Removing,
        DRAINED => Stage::Drained,
        DEAD => Stage::Dead,
        _ => return Err(errno(bindings::EINVAL)),
    };
    Ok(SafeState {
        stage,
        registered: raw.registered != 0,
        callback_drained: raw.callback_drained != 0,
        pending_len: if raw.publication_pending != 0 {
            Some(raw.pending_len)
        } else {
            None
        },
        external_avail: raw.external_avail,
    })
}

fn encode_safe(state: &SafeState) -> DriverState {
    DriverState {
        abi_version: ABI_VERSION,
        stage: match state.stage {
            Stage::Live => LIVE,
            Stage::Removing => REMOVING,
            Stage::Drained => DRAINED,
            Stage::Dead => DEAD,
        },
        registered: u32::from(state.registered),
        callback_drained: u32::from(state.callback_drained),
        publication_pending: u32::from(state.pending_len.is_some()),
        pending_len: state.pending_len.unwrap_or(0),
        external_avail: state.external_avail,
        reserved: 0,
    }
}

fn safe_transition(
    state: &mut SafeState,
    event: u32,
    value: u32,
) -> Result<DriverOutcome, i32> {
    let mut outcome = EMPTY_OUTCOME;
    match event {
        REGISTER if state.stage == Stage::Live && !state.registered => {
            state.registered = value != 0;
        }
        REGISTER if state.stage != Stage::Live => return Err(errno(bindings::ENODEV)),
        REGISTER => return Err(errno(bindings::EALREADY)),
        CALLBACK_COMPLETE
            if !state.callback_drained
                && value != 0
                && (state.stage == Stage::Live || state.stage == Stage::Removing) =>
        {
            state.pending_len = Some(value);
        }
        CALLBACK_COMPLETE => return Err(errno(bindings::EINVAL)),
        PUBLISH if !state.callback_drained => {
            let Some(length) = state.pending_len.take() else {
                return Err(errno(bindings::EINVAL));
            };
            state.external_avail = length;
        }
        PUBLISH => return Err(errno(bindings::EINVAL)),
        BEGIN_REMOVE if state.stage == Stage::Live => {
            state.stage = Stage::Removing;
            outcome.unregister_required = u32::from(state.registered);
            state.registered = false;
        }
        BEGIN_REMOVE => return Err(errno(bindings::EALREADY)),
        DRAIN if state.stage == Stage::Removing => {
            state.stage = Stage::Drained;
            state.callback_drained = true;
            state.pending_len = None;
        }
        DRAIN => return Err(errno(bindings::EINVAL)),
        FINAL_CLEAR if state.stage == Stage::Drained => state.external_avail = 0,
        FINAL_CLEAR => return Err(errno(bindings::EBUSY)),
        FINISH_REMOVE
            if state.stage == Stage::Drained
                && state.external_avail == 0
                && state.pending_len.is_none() =>
        {
            state.stage = Stage::Dead;
        }
        FINISH_REMOVE => return Err(errno(bindings::EBUSY)),
        _ => return Err(errno(bindings::EINVAL)),
    }
    Ok(outcome)
}

#[no_mangle]
unsafe extern "C" fn vrng_driver_rust_safe_step(
    state: *mut DriverState,
    event: u32,
    value: u32,
    outcome: *mut DriverOutcome,
) -> i32 {
    if !outcome.is_null() {
        // SAFETY: The ABI requires a writable outcome object.
        unsafe { outcome.write(EMPTY_OUTCOME) };
    }
    if state.is_null() || outcome.is_null() {
        return errno(bindings::EINVAL);
    }
    if event == INIT {
        let initialized = DriverState {
            abi_version: ABI_VERSION,
            stage: LIVE,
            registered: 0,
            callback_drained: 0,
            publication_pending: 0,
            pending_len: 0,
            external_avail: 0,
            reserved: 0,
        };
        // SAFETY: Null was rejected and the ABI grants exclusive access.
        unsafe { state.write(initialized) };
        return 0;
    }

    // SAFETY: The ABI grants one aligned state object for this call.
    let raw = unsafe { ptr::read(state) };
    let mut safe = match decode_safe(raw) {
        Ok(safe) => safe,
        Err(error) => return error,
    };
    let selected = match safe_transition(&mut safe, event, value) {
        Ok(selected) => selected,
        Err(error) => return error,
    };
    // SAFETY: State and outcome are exclusive, non-overlapping ABI objects.
    unsafe {
        state.write(encode_safe(&safe));
        outcome.write(selected);
    }
    0
}
