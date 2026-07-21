// SPDX-License-Identifier: GPL-2.0-or-later
// Expected failure: an allocator marked with its blocking effect cannot be
// called from an IRQ callback. Unmarked external allocators remain an audited
// effect-declaration boundary rather than something MC can infer.

#[may_sleep]
fn allocate_buffer() -> void {}

#[irq_context]
fn invalid_completion() -> void {
    allocate_buffer();
}
