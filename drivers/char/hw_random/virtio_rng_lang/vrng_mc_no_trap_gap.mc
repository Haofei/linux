// SPDX-License-Identifier: GPL-2.0-or-later

// Expected compile failure documenting the current MC FFI/no-trap gap.
// A full-domain u32 load from an extern C-layout struct still creates an
// InvalidRepresentation trap edge, so a callback cannot yet prove no_lang_trap.

extern struct ProbeState {
    value: u32,
}

#[no_lang_trap]
fn reject_extern_state_read(state: *const ProbeState) -> u32 {
    // EXPECT_ERROR: E_NO_LANG_TRAP_EDGE
    return state.value;
}
