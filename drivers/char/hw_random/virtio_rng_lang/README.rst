========================================
Virtio RNG language core experiment
========================================

Scope
=====

This directory compares C, Rust, and MC implementations of the logical
single-buffer virtio-rng protocol.  The production ``virtio-rng.c`` driver is
not changed during the specification and KUnit milestones.

The common C/Linux glue will continue to own allocation, DMA grouping and
mapping, virtqueue operations, callback entry, completion wakeups, PM, and
object lifetime.  Consequently the initial experiment measures a logical
ownership protocol, not language-enforced ownership of the physical DMA
allocation.

Concurrency contract
====================

The glue must serialize every mutable call for a given ``vrng_core_state``.
This is also the unsafe precondition that permits the Rust implementation to
construct one exclusive reference from the FFI pointer.  ``complete`` is
IRQ-callable and therefore must not allocate, block, execute an unbounded loop,
panic, or take a language trap.

The state has independent lifecycle and buffer dimensions::

  Lifecycle: Active | Quiescing | Dead
  Buffer:    Empty | DeviceOwned(generation) | Ready(index, available)

``Quiescing`` prevents new reads and submissions but does not claim that reset
has drained the virtqueue.  The glue calls ``finish_remove`` only after reset
and ``del_vqs``; only then is the lifecycle ``Dead``.

Submission is transactional.  ``begin_submit`` logically transfers the buffer
before the glue publishes a descriptor.  A failed ``virtqueue_add_inbuf`` is
followed by ``abort_submit``.  The queue token carries the returned generation;
the callback never reconstructs it from current mutable state.

Errors
======

The ABI uses the following stable meanings:

``-EINVAL``
  Null pointer, zero capacity, unknown ABI/state value, or broken invariant.

``-ENODEV``
  An operation requires an Active device, or completion arrived while the
  device was quiescing/dead.

``-EAGAIN``
  Copy was requested while the active buffer was Empty.

``-EBUSY``
  Copy was requested while the device owns the buffer, or submission was
  requested while a buffer was already outstanding/ready.

``-EALREADY``
  The requested transition has already happened, including a duplicate
  completion or repeated lifecycle transition.

``-ESTALE``
  A completion or rollback cookie does not match the outstanding generation.

``-ENODATA``
  The device completed a valid request with zero bytes.  The descriptor was
  consumed and a resubmit is requested.

``-EOVERFLOW``
  Checked generation/index arithmetic overflowed, or the device reported more
  bytes than the posted capacity.  An oversize completion consumes the
  descriptor and requests a resubmit.

Observable invariants
=====================

For all valid states::

  capacity > 0
  data_idx <= capacity
  data_avail <= capacity
  data_idx + data_avail <= capacity, without wrapping

Empty and DeviceOwned have zero index and availability.  Ready is Active and
has nonzero availability.  Quiescing is never Ready.  Dead is always Empty.

All output parameters are initialized before a transition-specific error is
returned.  Rejected transitions do not partially mutate the state, except that
a valid zero/oversize completion consumes DeviceOwned and returns to Empty as
documented above.

Testing
=======

Current status (2026-07-20)
---------------------------

The executable specification and all three candidates are implemented.  The
base KUnit kernel executes twelve tests: eight C/spec tests and directed plus
depth-seven bounded state-space comparisons for both Rust and MC.  All twelve
pass on x86-64, arm64, and riscv64 QEMU kernels.  Shadow-enabled kernels add
three tests for normal mirroring, queue-add rollback, and bounded mismatch
recording; all fifteen pass on x86-64, arm64, and riscv64.  They also pass under
KCSAN and under a combined KASAN/UBSAN/lockdep/debug-atomic-sleep x86-64
configuration.  C, Rust, and MC objects cross-build for all three
architectures; ``mcc emit-layout`` reports the expected 40-byte MC C-ABI layout.

The M0 object-toolchain gate is now closed.  MC's ``--linux-kernel`` LLVM
profile emits x86 IBT and return-thunk metadata, marks functions ``nounwind``,
and externalizes runtime hooks.  The resulting x86 object is objtool-clean,
has no ``.eh_frame`` or hidden MC helper text, and has only the expected kernel
``__x86_return_thunk`` reference.  arm64 and riscv64 objects have no undefined
symbols.  The first arm64 runtime run found missing BTI landing pads in the MC
object; the Linux LLVM profile now emits arm64 branch-target-enforcement
attributes and metadata, and the passing object begins exported functions with
``bti c``.  Kbuild tracks the private ``mcc-real`` executable as well as its
launcher so compiler updates invalidate generated objects.

M3 shadow mode is implemented beside ``virtio-rng.c``.  The production driver
remains authoritative for all queue, DMA, copy, wakeup, and lifecycle behavior.
A spinlock-serialized, preallocated shadow mirrors submit, abort, completion,
copy, and two-stage removal into independent C, Rust, and MC states.  Callback
handling only updates fixed storage; detailed reporting is deferred until
device removal.  Two concurrent ``/dev/hwrng`` readers followed by virtio
unbind produced 59,774 mirrored events and zero mismatches in normal, KCSAN,
and KASAN/UBSAN/lockdep kernels.

Shadow mode is not candidate-control mode.  ``#[irq_context]`` verifies the MC
completion call graph, but ``#[no_lang_trap]`` cannot certify access to the
C-layout state: current MC inserts ``InvalidRepresentation`` edges for even
full-domain ``u32`` fields in an ``extern struct``.  The minimal expected-failure
fixture is ``vrng_mc_no_trap_gap.mc``.  Optimized machine code contains no trap
call, but that is weaker evidence than satisfying the source contract and
remains a recorded language gap.  The Rust copy path calls the common C
boundary's scalar ``array_index_nospec`` wrapper because the kernel primitive
is a C macro/static inline and has no Rust abstraction or bindgen symbol.

Run the differential suite under QEMU with::

  ./tools/testing/kunit/kunit.py run virtio-rng-lang-core \
    --arch=x86_64 --make_options LLVM=1 \
    --make_options MCC=/absolute/path/to/mcc \
    --kunitconfig drivers/char/hw_random/virtio_rng_lang

The suite covers directed boundary cases and a breadth-first exploration of
the event graph through depth seven at capacity three.  Every explored event
compares the executable specification with each enabled candidate's return
code, full state, outputs, and copied bytes.

The ABI and C, Rust, and MC sources are cross-built and runtime-tested with LLVM
for arm64 and riscv64.

Remaining gates before candidate control
========================================

1. Decide whether to fix MC's extern-struct representation proof so the IRQ
   completion path can satisfy ``#[no_lang_trap]``; otherwise retain it as a
   measured language limitation.
2. Add host differential enumeration with failure-sequence persistence.
3. Add live fault-injection cases without weakening the production kernel
   configuration.
4. Exercise suspend/restore and transport-level hot-unplug races.  Candidate
   control remains disabled until those cases pass KCSAN and QEMU stress.

MC contract fixtures
====================

The following source files are intentionally rejected by ``mcc verify`` and
pin the callback contract diagnostics independently of the candidate:

* ``vrng_mc_irq_blocking_gap.mc``: ``E_SLEEP_IN_ATOMIC``;
* ``vrng_mc_irq_unbounded_gap.mc``: ``E_UNBOUNDED_LOOP``;
* ``vrng_mc_no_trap_gap.mc``: ``E_NO_LANG_TRAP_EDGE`` (an observed language
  limitation, not a desired rejection).
