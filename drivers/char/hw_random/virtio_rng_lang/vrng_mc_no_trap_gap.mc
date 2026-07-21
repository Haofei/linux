// SPDX-License-Identifier: GPL-2.0-or-later

// Qualification fixture: a nullable C-ABI pointer is checked at the boundary,
// and the narrowed non-null binding can read a full-domain extern-struct field
// without introducing an InvalidRepresentation trap edge.

extern struct ProbeState {
    value: u32,
}

#[no_lang_trap]
fn accept_extern_state_read(maybe_state: ?*const ProbeState) -> u32 {
    if let state = maybe_state {
        return state.value;
    }
    return 0;
}
