/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _VRNG_CORE_ABI_H
#define _VRNG_CORE_ABI_H

#include <linux/build_bug.h>
#include <linux/stddef.h>
#include <linux/types.h>

#define VRNG_CORE_ABI_VERSION 1U

enum vrng_lifecycle {
	VRNG_ACTIVE = 0,
	VRNG_QUIESCING = 1,
	VRNG_DEAD = 2,
};

enum vrng_buffer_phase {
	VRNG_EMPTY = 0,
	VRNG_DEVICE_OWNED = 1,
	VRNG_READY = 2,
};

/*
 * Pointer-only C ABI shared by the C, Rust, and MC implementations.
 *
 * The experiment targets 64-bit x86, arm64, and riscv64.  Explicit alignment
 * and assertions make accidental layout drift a build failure instead of an
 * FFI bug.  The common C glue serializes all mutable calls to one state object.
 */
struct vrng_core_state {
	u32 abi_version;
	u32 lifecycle;
	u32 phase;
	u32 capacity;
	u32 data_idx;
	u32 data_avail;
	u64 epoch;
	u64 generation;
} __aligned(8);

static_assert(offsetof(struct vrng_core_state, abi_version) == 0);
static_assert(offsetof(struct vrng_core_state, lifecycle) == 4);
static_assert(offsetof(struct vrng_core_state, phase) == 8);
static_assert(offsetof(struct vrng_core_state, capacity) == 12);
static_assert(offsetof(struct vrng_core_state, data_idx) == 16);
static_assert(offsetof(struct vrng_core_state, data_avail) == 20);
static_assert(offsetof(struct vrng_core_state, epoch) == 24);
static_assert(offsetof(struct vrng_core_state, generation) == 32);
static_assert(sizeof(struct vrng_core_state) == 40);
static_assert(__alignof__(struct vrng_core_state) == 8);

u32 vrng_core_index_nospec(u32 index, u32 size);

int vrng_core_c_init(struct vrng_core_state *state, u32 capacity, u64 epoch);
int vrng_core_c_begin_submit(struct vrng_core_state *state, u64 *generation);
int vrng_core_c_abort_submit(struct vrng_core_state *state, u64 generation);
int vrng_core_c_complete(struct vrng_core_state *state, u64 generation,
			 u32 produced, u32 *need_resubmit);
int vrng_core_c_copy(struct vrng_core_state *state, const u8 *dma_buffer,
		     u8 *destination, u32 requested, u32 *copied,
		     u32 *need_resubmit);
int vrng_core_c_begin_remove(struct vrng_core_state *state);
int vrng_core_c_finish_remove(struct vrng_core_state *state);
int vrng_core_c_validate(const struct vrng_core_state *state);

int vrng_core_rust_init(struct vrng_core_state *state, u32 capacity, u64 epoch);
int vrng_core_rust_begin_submit(struct vrng_core_state *state, u64 *generation);
int vrng_core_rust_abort_submit(struct vrng_core_state *state, u64 generation);
int vrng_core_rust_complete(struct vrng_core_state *state, u64 generation,
			    u32 produced, u32 *need_resubmit);
int vrng_core_rust_copy(struct vrng_core_state *state, const u8 *dma_buffer,
			u8 *destination, u32 requested, u32 *copied,
			u32 *need_resubmit);
int vrng_core_rust_begin_remove(struct vrng_core_state *state);
int vrng_core_rust_finish_remove(struct vrng_core_state *state);
int vrng_core_rust_validate(const struct vrng_core_state *state);

int vrng_core_mc_init(struct vrng_core_state *state, u32 capacity, u64 epoch);
int vrng_core_mc_begin_submit(struct vrng_core_state *state, u64 *generation);
int vrng_core_mc_abort_submit(struct vrng_core_state *state, u64 generation);
int vrng_core_mc_complete(struct vrng_core_state *state, u64 generation,
			  u32 produced, u32 *need_resubmit);
int vrng_core_mc_copy(struct vrng_core_state *state, const u8 *dma_buffer,
		      u8 *destination, u32 requested, u32 *copied,
		      u32 *need_resubmit);
int vrng_core_mc_begin_remove(struct vrng_core_state *state);
int vrng_core_mc_finish_remove(struct vrng_core_state *state);
int vrng_core_mc_validate(const struct vrng_core_state *state);

#endif /* _VRNG_CORE_ABI_H */
