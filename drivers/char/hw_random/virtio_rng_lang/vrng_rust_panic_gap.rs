// SPDX-License-Identifier: GPL-2.0-or-later

// Deliberate defect fixture. Rust can express a panic-capable callback; kernel
// panic policy and review are not a type-system prevention. The production
// candidate is required to remain free of panic!/unwrap()/expect().
pub extern "C" fn invalid_completion() {
    panic!("deliberate virtio-rng callback defect");
}
