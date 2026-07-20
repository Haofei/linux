// SPDX-License-Identifier: GPL-2.0-or-later
// Expected failure: an IRQ callback must not call a sleepable operation.

#[may_sleep]
fn wait_for_entropy() -> void {}

#[irq_context]
fn invalid_completion() -> void {
    wait_for_entropy();
}
