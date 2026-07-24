// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/errno.h>
#include <linux/string.h>

#include "vrng_driver_abi.h"

/*
 * Independent executable specification. Keep this transition table separate
 * from every candidate so candidate agreement cannot redefine correctness.
 */
int vrng_driver_spec_step(struct vrng_driver_state *state, u32 event, u32 value,
			  struct vrng_driver_outcome *outcome)
{
	if (outcome)
		*outcome = (struct vrng_driver_outcome) {};
	if (!state || !outcome)
		return -EINVAL;
	if (event == VRNG_DRIVER_INIT) {
		memset(state, 0, sizeof(*state));
		state->abi_version = VRNG_DRIVER_ABI_VERSION;
		state->stage = VRNG_DRIVER_LIVE;
		return 0;
	}
	if (state->abi_version != VRNG_DRIVER_ABI_VERSION ||
	    state->stage < VRNG_DRIVER_LIVE ||
	    state->stage > VRNG_DRIVER_DEAD || state->registered > 1 ||
	    state->callback_drained > 1 || state->publication_pending > 1 ||
	    state->publication_pending != !!state->pending_len)
		return -EINVAL;

	if (event == VRNG_DRIVER_REGISTER) {
		if (state->stage != VRNG_DRIVER_LIVE)
			return -ENODEV;
		if (state->registered)
			return -EALREADY;
		state->registered = value != 0;
		return 0;
	}
	if (event == VRNG_DRIVER_CALLBACK_COMPLETE) {
		if (state->callback_drained || !value ||
		    (state->stage != VRNG_DRIVER_LIVE &&
		     state->stage != VRNG_DRIVER_REMOVING))
			return -EINVAL;
		state->publication_pending = 1;
		state->pending_len = value;
		return 0;
	}
	if (event == VRNG_DRIVER_PUBLISH) {
		if (!state->publication_pending || state->callback_drained)
			return -EINVAL;
		state->external_avail = state->pending_len;
		state->publication_pending = 0;
		state->pending_len = 0;
		return 0;
	}
	if (event == VRNG_DRIVER_BEGIN_REMOVE) {
		if (state->stage != VRNG_DRIVER_LIVE)
			return -EALREADY;
		outcome->unregister_required = state->registered;
		state->registered = 0;
		state->stage = VRNG_DRIVER_REMOVING;
		return 0;
	}
	if (event == VRNG_DRIVER_DRAIN) {
		if (state->stage != VRNG_DRIVER_REMOVING)
			return -EINVAL;
		state->stage = VRNG_DRIVER_DRAINED;
		state->callback_drained = 1;
		state->publication_pending = 0;
		state->pending_len = 0;
		return 0;
	}
	if (event == VRNG_DRIVER_FINAL_CLEAR) {
		if (state->stage != VRNG_DRIVER_DRAINED)
			return -EBUSY;
		state->external_avail = 0;
		return 0;
	}
	if (event == VRNG_DRIVER_FINISH_REMOVE) {
		if (state->stage != VRNG_DRIVER_DRAINED ||
		    state->external_avail || state->publication_pending)
			return -EBUSY;
		state->stage = VRNG_DRIVER_DEAD;
		return 0;
	}
	return -EINVAL;
}
