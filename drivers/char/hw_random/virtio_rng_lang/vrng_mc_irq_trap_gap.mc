// SPDX-License-Identifier: GPL-2.0-or-later
// Expected failure: a qualified callback must have no language trap edge.

#[irq_context]
#[no_lang_trap]
fn invalid_completion() -> void {
    unreachable;
}
