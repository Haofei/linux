// SPDX-License-Identifier: GPL-2.0-or-later
// Deliberate defect: wrapping arithmetic is expressible when explicitly chosen.
// The language does not prevent this policy error; the executable-spec
// publication firewall must reject the resulting transition divergence.

fn invalid_remaining(index: u32, available: u32) -> u32 {
    return wrapping.add(index, available);
}
