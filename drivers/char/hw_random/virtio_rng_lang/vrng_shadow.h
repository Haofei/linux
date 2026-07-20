/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _VRNG_SHADOW_H
#define _VRNG_SHADOW_H

#include <linux/spinlock.h>

#include "vrng_core_abi.h"

#define VRNG_SHADOW_MAX_COPY 256U

enum vrng_shadow_event {
	VRNG_SHADOW_INIT = 1,
	VRNG_SHADOW_BEGIN_SUBMIT,
	VRNG_SHADOW_ABORT_SUBMIT,
	VRNG_SHADOW_COMPLETE,
	VRNG_SHADOW_COPY,
	VRNG_SHADOW_BEGIN_REMOVE,
	VRNG_SHADOW_FINISH_REMOVE,
};

struct vrng_shadow_mismatch {
	u64 sequence;
	u32 event;
	s32 c_result;
	s32 rust_result;
	s32 mc_result;
	u32 c_output;
	u32 rust_output;
	u32 mc_output;
	struct vrng_core_state c_state;
	struct vrng_core_state rust_state;
	struct vrng_core_state mc_state;
};

struct vrng_shadow {
	/* Serializes process-context copies/removal against completion IRQs. */
	spinlock_t lock;
	struct vrng_core_state c_state;
	struct vrng_core_state rust_state;
	struct vrng_core_state mc_state;
	u64 generation;
	u64 sequence;
	u64 mismatches;
	struct vrng_shadow_mismatch last_mismatch;
	bool active;
};

int vrng_shadow_init(struct vrng_shadow *shadow, u32 capacity);
int vrng_shadow_begin_submit(struct vrng_shadow *shadow, u64 *generation);
int vrng_shadow_abort_submit(struct vrng_shadow *shadow, u64 generation);
int vrng_shadow_recover_consumed(struct vrng_shadow *shadow);
int vrng_shadow_complete(struct vrng_shadow *shadow, u64 generation,
			 u32 produced, u32 *need_resubmit);
int vrng_shadow_copy(struct vrng_shadow *shadow, const u8 *dma_buffer,
		     u8 *destination, u32 requested, u32 *copied,
		     u32 *need_resubmit);
int vrng_shadow_begin_remove(struct vrng_shadow *shadow);
int vrng_shadow_finish_remove(struct vrng_shadow *shadow);
void vrng_shadow_snapshot(struct vrng_shadow *shadow, u64 *events,
			  u64 *mismatches,
			  struct vrng_shadow_mismatch *last);

#endif /* _VRNG_SHADOW_H */
