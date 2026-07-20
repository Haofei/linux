/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _VRNG_CORE_SPEC_H
#define _VRNG_CORE_SPEC_H

#include "vrng_core_abi.h"

enum vrng_spec_event_kind {
	VRNG_EVENT_INIT,
	VRNG_EVENT_BEGIN_SUBMIT,
	VRNG_EVENT_ABORT_SUBMIT,
	VRNG_EVENT_COMPLETE,
	VRNG_EVENT_COPY,
	VRNG_EVENT_BEGIN_REMOVE,
	VRNG_EVENT_FINISH_REMOVE,
	VRNG_EVENT_VALIDATE,
};

struct vrng_spec_event {
	u32 kind;
	u32 value;
	u64 generation;
	u64 epoch;
};

struct vrng_spec_outcome {
	s32 result;
	u32 copied;
	u32 need_resubmit;
	u64 generation;
};

int vrng_spec_step(struct vrng_core_state *state,
		   const struct vrng_spec_event *event, const u8 *dma_buffer,
		   u8 *destination, struct vrng_spec_outcome *outcome);

#endif /* _VRNG_CORE_SPEC_H */
