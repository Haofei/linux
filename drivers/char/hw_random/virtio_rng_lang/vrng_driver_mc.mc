// SPDX-License-Identifier: GPL-2.0-or-later

// Raw-ABI and typed-contract MC candidates for common driver lifecycle policy.

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

const EINVAL: i32 = - 22;
const ENODEV: i32 = - 19;
const EBUSY: i32 = - 16;
const EALREADY: i32 = - 114;

extern struct DriverState {
    abi_version: u32,
    stage: u32,
    registered: u32,
    callback_drained: u32,
    publication_pending: u32,
    pending_len: u32,
    external_avail: u32,
    reserved: u32,
}

extern struct DriverOutcome {
    unregister_required: u32,
    reserved: u32,
}

open enum ContractStage: u32 {
    Live = 1,
    Removing = 2,
    Drained = 3,
    Dead = 4,
}

#[irq_context]
#[no_lang_trap]
fn zero_outcome(maybe_outcome: ?*mut DriverOutcome) -> void {
    if let outcome = maybe_outcome {
        outcome.unregister_required = 0;
        outcome.reserved = 0;
    }
}

#[irq_context]
#[no_lang_trap]
fn initialize(maybe_state: ?*mut DriverState) -> void {
    if let state = maybe_state {
        state.abi_version = ABI_VERSION;
        state.stage = LIVE;
        state.registered = 0;
        state.callback_drained = 0;
        state.publication_pending = 0;
        state.pending_len = 0;
        state.external_avail = 0;
        state.reserved = 0;
    }
}

#[irq_context]
#[no_lang_trap]
fn valid(maybe_state: ?*const DriverState) -> bool {
    if let state = maybe_state {
        if state.abi_version != ABI_VERSION || state.stage > DEAD {
            return false;
        }
        if state.registered > 1 || state.callback_drained > 1 ||
            state.publication_pending > 1 {
            return false;
        }
        if state.publication_pending == 0 && state.pending_len != 0 {
            return false;
        }
        if state.publication_pending == 1 && state.pending_len == 0 {
            return false;
        }
        if state.stage == LIVE && state.callback_drained != 0 {
            return false;
        }
        if state.stage >= DRAINED {
            if state.callback_drained == 0 || state.registered != 0 ||
                state.publication_pending != 0 {
                return false;
            }
        }
        if state.stage == DEAD && state.external_avail != 0 {
            return false;
        }
        return true;
    }
    return false;
}

#[irq_context]
#[no_lang_trap]
fn raw_transition(
    maybe_state: ?*mut DriverState,
    event: u32,
    value: u32,
    maybe_outcome: ?*mut DriverOutcome,
) -> i32 {
  if let outcome = maybe_outcome {
    if let state = maybe_state {
    if event == REGISTER {
        if state.stage != LIVE { return ENODEV; }
        if state.registered != 0 { return EALREADY; }
        if value != 0 { state.registered = 1; }
        return 0;
    }
    if event == CALLBACK_COMPLETE {
        if value == 0 || state.callback_drained != 0 {
            return EINVAL;
        }
        if state.stage != LIVE && state.stage != REMOVING {
            return EINVAL;
        }
        state.publication_pending = 1;
        state.pending_len = value;
        return 0;
    }
    if event == PUBLISH {
        if state.publication_pending == 0 || state.callback_drained != 0 {
            return EINVAL;
        }
        state.external_avail = state.pending_len;
        state.publication_pending = 0;
        state.pending_len = 0;
        return 0;
    }
    if event == BEGIN_REMOVE {
        if state.stage != LIVE { return EALREADY; }
        state.stage = REMOVING;
        outcome.unregister_required = state.registered;
        state.registered = 0;
        return 0;
    }
    if event == DRAIN {
        if state.stage != REMOVING { return EINVAL; }
        state.stage = DRAINED;
        state.callback_drained = 1;
        state.publication_pending = 0;
        state.pending_len = 0;
        return 0;
    }
    if event == FINAL_CLEAR {
        if state.stage != DRAINED { return EBUSY; }
        state.external_avail = 0;
        return 0;
    }
    if event == FINISH_REMOVE {
        if state.stage != DRAINED || state.external_avail != 0 ||
            state.publication_pending != 0 {
            return EBUSY;
        }
        state.stage = DEAD;
        return 0;
    }
    return EINVAL;
    }
  }
  return EINVAL;
}

#[irq_context]
#[no_lang_trap]
export fn vrng_driver_mc_raw_step(
    maybe_state: ?*mut DriverState,
    event: u32,
    value: u32,
    maybe_outcome: ?*mut DriverOutcome,
) -> i32 {
    zero_outcome(maybe_outcome);
    if let outcome = maybe_outcome {
        if let state = maybe_state {
            if event == INIT {
                initialize(state);
                return 0;
            }
            if !valid(state as ?*const DriverState) { return EINVAL; }
            return raw_transition(state, event, value, outcome);
        }
    }
    return EINVAL;
}

#[irq_context]
#[no_lang_trap]
fn contract_stage(raw: u32) -> ContractStage {
    switch raw {
        1 => { return.Live; }
        2 => { return.Removing; }
        3 => { return.Drained; }
        _ => { return.Dead; }
    }
}

#[irq_context]
#[no_lang_trap]
export fn vrng_driver_mc_contract_step(
    maybe_state: ?*mut DriverState,
    event: u32,
    value: u32,
    maybe_outcome: ?*mut DriverOutcome,
) -> i32 {
    zero_outcome(maybe_outcome);
    if let outcome = maybe_outcome {
        if let state = maybe_state {
            if event == INIT {
                initialize(state);
                return 0;
            }
            if !valid(state as ?*const DriverState) || state.stage == RESET {
                return EINVAL;
            }
            var stage: ContractStage = contract_stage(state.stage);
            var registered: bool = state.registered != 0;
            var drained: bool = state.callback_drained != 0;
            var pending_len: u32 = state.pending_len;
            var external_avail: u32 = state.external_avail;

            if event == REGISTER {
                if stage !=.Live { return ENODEV; }
                if registered { return EALREADY; }
                registered = value != 0;
            } else if event == CALLBACK_COMPLETE {
                if drained || value == 0 { return EINVAL; }
                if stage !=.Live && stage !=.Removing { return EINVAL; }
                pending_len = value;
            } else if event == PUBLISH {
                if drained || pending_len == 0 { return EINVAL; }
                external_avail = pending_len;
                pending_len = 0;
            } else if event == BEGIN_REMOVE {
                if stage !=.Live { return EALREADY; }
                stage =.Removing;
                if registered { outcome.unregister_required = 1; }
                registered = false;
            } else if event == DRAIN {
                if stage !=.Removing { return EINVAL; }
                stage =.Drained;
                drained = true;
                pending_len = 0;
            } else if event == FINAL_CLEAR {
                if stage !=.Drained { return EBUSY; }
                external_avail = 0;
            } else if event == FINISH_REMOVE {
                if stage !=.Drained || external_avail != 0 ||
                    pending_len != 0 {
                    return EBUSY;
                }
                stage =.Dead;
            } else {
                return EINVAL;
            }

            state.stage = stage as u32;
            if registered { state.registered = 1; } else { state.registered = 0; }
            if drained { state.callback_drained = 1; } else { state.callback_drained = 0; }
            if pending_len != 0 {
                state.publication_pending = 1;
            } else {
                state.publication_pending = 0;
            }
            state.pending_len = pending_len;
            state.external_avail = external_avail;
            return 0;
        }
    }
    return EINVAL;
}
