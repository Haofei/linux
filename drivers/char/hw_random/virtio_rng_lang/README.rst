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
construct one exclusive reference from the FFI pointer.  ``complete`` and
``abort_submit`` are IRQ-callable and therefore must not allocate, block,
execute an unbounded loop, panic, or take a language trap.

The state has independent lifecycle and buffer dimensions::

  Lifecycle: Active | Quiescing | Dead
  Buffer:    Empty | DeviceOwned(generation) | Ready(index, available)

``Quiescing`` prevents new reads and submissions but does not claim that reset
has drained the virtqueue.  The glue calls ``finish_remove`` only after reset
and ``del_vqs``; only then is the lifecycle ``Dead``.

Submission is transactional.  ``begin_submit`` logically transfers the buffer
before the glue publishes a descriptor.  A failed ``virtqueue_add_inbuf`` is
followed by ``abort_submit`` and is propagated without kicking the queue.  The
queue token is an embedded cookie containing the device, epoch, generation, and
request identifier; its contents remain immutable while the descriptor is
queued.  The callback passes the cookie generation to the core and never
reconstructs it from current mutable state.

This identity contract trusts the virtqueue to map one used entry to its
currently outstanding token.  It detects stale generations presented with a
valid token; it does not claim to distinguish arbitrary used-ring identifier
replay after descriptor/token reuse.  Such transport-level protocol violations
require a separate fault model and are not covered by the local stale-generation
injection.

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

Every non-NULL output parameter is initialized before the complete output set,
state representation, data pointers, lifecycle, or phase is validated, in that
order.  Rejected transitions do not partially mutate the state, except that a
valid zero/oversize completion consumes DeviceOwned and returns to Empty as
documented above.  The shared ABI header defines alignment, extent,
non-aliasing, serialization, and IRQ-context requirements.

Testing
=======

Current status (2026-07-20)
---------------------------

The executable specification and all three candidates are implemented.  The
published baseline ran twelve tests on x86-64, arm64, and riscv64 QEMU kernels,
plus three shadow tests, and passed under KCSAN and a combined
KASAN/UBSAN/lockdep/debug-atomic-sleep x86-64 configuration.  The M3.5 patch
adds per-language pointer/state/output contract tests and a queued-cookie
generation test.  The normal x86-64 configuration passes all 23 KUnit tests;
the C-only shadow-disabled configuration passes 11/11, including persistent
fatal-error visibility, controlling-output validation, absolute-index,
and remaining-length partial-copy accounting.  C, Rust, and MC objects
cross-build for all three architectures;
``mcc emit-layout`` reports the expected 40-byte MC C-ABI layout.

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

M3 normal-path shadow mode is implemented beside ``virtio-rng.c``.  Its
published run compared the three language models while the original production
logic remained authoritative; it did not establish equivalence with that live
logic.  Two concurrent ``/dev/hwrng`` readers followed by virtio unbind produced
59,774 mirrored events and zero language-model mismatches in normal, KCSAN, and
KASAN/UBSAN/lockdep kernels.

M3.5 makes the experimental C core authoritative for live logical completion,
copy, and resubmission decisions while Rust and MC remain shadows.  Common C
glue owns the queue and DMA storage, propagates queue-add errors, and uses
generation cookies whose contents remain immutable while queued.  It resubmits
zero/oversize completions from a preallocated work item after the reader
observes the stored error and serializes process copy/resubmit against removal.
Fatal errors remain visible to every later reader, probe and restore failures
clear published ownership state, and restore registration state is synchronized
with readers and removal.  Copy comparison stages all three implementations in
private canary buffers and publishes only a C result bounded by the request and
pre-call available bytes.  Every controlling transition is also run against an
independent executable-specification state; return code, outputs, post-state,
and copied bytes must match before the result can affect the live driver.
The normal x86-64 live PCI tests passed a forced three-byte driver copy limit in
both shadow-disabled and shadow builds.  The shadow run reached the held-
completion synchronization point before unbind with 1,213 matching protocol
events.  KCSAN and the combined KASAN/UBSAN/lockdep/DMA-debug live runs passed
with 1,213 and 1,216 matching events, respectively.  The deterministic live
fault matrix recovered from zero-length and oversized completions, a stale
generation, and one queue-add failure with 1,243 matching events.  The full
23-test KUnit suite also passes on x86-64, arm64, and riscv64.  A PM-debug live
run completed three device-level suspend/restore cycles, restored live reads
after each cycle, and then passed synchronized unbind with zero mismatches; the
same matrix also passes under KCSAN.  A QMP-controlled PCI hot-unplug terminated
a reader blocked behind a held completion, removed the transport, re-added a
fresh virtio-rng device, restored live reads, and then passed the final
synchronized unbind with zero mismatches.  The host differential gate links the
executable specification and all three actual implementations, explores 30
unique states, replays committed corpora, and proves deterministic failure
capture with an injected mismatch.

Rust and MC remain shadows rather than selectable controlling cores.
``#[irq_context]`` and ``#[no_lang_trap]`` now verify the MC completion call
graph.  Nullable ABI pointers are explicitly narrowed before C-layout state
access; the successful branch carries a scoped nonnull representation proof.
``vrng_mc_no_trap_gap.mc`` is now a positive qualification fixture, while a
direct unchecked nonnull parameter remains rejected by the compiler regression.
The Rust copy path calls the common C
boundary's scalar ``array_index_nospec`` wrapper because the kernel primitive
is a C macro/static inline and has no Rust abstraction or bindgen symbol.

Run the differential suite under QEMU with::

  ./tools/testing/kunit/kunit.py run virtio-rng-lang-core \
    --arch=x86_64 --make_options LLVM=1 \
    --make_options MCC=/absolute/path/to/mcc \
    --kunitconfig drivers/char/hw_random/virtio_rng_lang

Use ``virtio_rng_lang/kunitconfig-no-shadow`` to compile and exercise the
ordinary driver branch without Rust, MC, or shadow control.

The suite covers directed boundary cases and a breadth-first exploration of
the event graph through depth seven at capacity three.  Every explored event
compares the executable specification with each enabled candidate's return
code, full state, outputs, and copied bytes.

The ABI and C, Rust, and MC sources are cross-built and runtime-tested with LLVM
for arm64 and riscv64.

Remaining gates before candidate control
========================================

1. Preserve the completed M3.5 normal, KCSAN, KASAN/UBSAN/lockdep, three-arch
   KUnit, live queue/completion-fault, PM, and QMP hotplug gates in reproducible
   result manifests.
2. Add the selectable C/Rust/MC controlling-core choice and run the full fault
   and lifecycle matrix independently for each implementation.

MC contract fixtures
====================

The following source files pin the callback contract independently of the
candidate:

* ``vrng_mc_irq_blocking_gap.mc``: ``E_SLEEP_IN_ATOMIC``;
* ``vrng_mc_irq_unbounded_gap.mc``: ``E_UNBOUNDED_LOOP``;
* ``vrng_mc_no_trap_gap.mc``: accepted nullable-pointer narrowing and extern-
  struct access under ``#[no_lang_trap]``.
