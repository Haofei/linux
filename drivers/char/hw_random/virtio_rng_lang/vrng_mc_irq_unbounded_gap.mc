// SPDX-License-Identifier: GPL-2.0-or-later
// Expected failure: an IRQ callback must have statically bounded control flow.

#[irq_context]
fn invalid_completion() -> void {
    while true {
    }
}
