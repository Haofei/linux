/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _VRNG_DRIVER_ABI_H
#define _VRNG_DRIVER_ABI_H

#include <linux/build_bug.h>
#include <linux/types.h>

#define VRNG_DRIVER_ABI_VERSION 1U

enum vrng_driver_stage {
	VRNG_DRIVER_RESET = 0,
	VRNG_DRIVER_LIVE = 1,
	VRNG_DRIVER_REMOVING = 2,
	VRNG_DRIVER_DRAINED = 3,
	VRNG_DRIVER_DEAD = 4,
};

enum vrng_driver_event {
	VRNG_DRIVER_INIT = 1,
	VRNG_DRIVER_REGISTER = 2,
	VRNG_DRIVER_CALLBACK_COMPLETE = 3,
	VRNG_DRIVER_PUBLISH = 4,
	VRNG_DRIVER_BEGIN_REMOVE = 5,
	VRNG_DRIVER_DRAIN = 6,
	VRNG_DRIVER_FINAL_CLEAR = 7,
	VRNG_DRIVER_FINISH_REMOVE = 8,
};

/*
 * Complete logical lifecycle state at the language boundary. Linux allocation,
 * virtqueue reset/drain, hwrng registration calls, and external stores remain
 * physical effects in common C. The selected language decides whether those
 * effects are legal and whether unregister is required.
 */
struct vrng_driver_state {
	u32 abi_version;
	u32 stage;
	u32 registered;
	u32 callback_drained;
	u32 publication_pending;
	u32 pending_len;
	u32 external_avail;
	u32 reserved;
};

struct vrng_driver_outcome {
	u32 unregister_required;
	u32 reserved;
};

static_assert(sizeof(struct vrng_driver_state) == 32);
static_assert(sizeof(struct vrng_driver_outcome) == 8);

int vrng_driver_c_step(struct vrng_driver_state *state, u32 event, u32 value,
		       struct vrng_driver_outcome *outcome);
int vrng_driver_rust_raw_step(struct vrng_driver_state *state, u32 event,
			      u32 value,
			      struct vrng_driver_outcome *outcome);
int vrng_driver_rust_safe_step(struct vrng_driver_state *state, u32 event,
			       u32 value,
			       struct vrng_driver_outcome *outcome);
int vrng_driver_mc_raw_step(struct vrng_driver_state *state, u32 event,
			    u32 value,
			    struct vrng_driver_outcome *outcome);
int vrng_driver_mc_contract_step(struct vrng_driver_state *state, u32 event,
				 u32 value,
				 struct vrng_driver_outcome *outcome);
int vrng_driver_spec_step(struct vrng_driver_state *state, u32 event, u32 value,
			  struct vrng_driver_outcome *outcome);

#endif
