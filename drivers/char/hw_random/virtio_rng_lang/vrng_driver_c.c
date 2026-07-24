// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/errno.h>
#include <linux/string.h>

#include "vrng_driver_abi.h"

static int vrng_driver_c_valid(const struct vrng_driver_state *state)
{
	if (state->abi_version != VRNG_DRIVER_ABI_VERSION ||
	    state->stage > VRNG_DRIVER_DEAD || state->registered > 1 ||
	    state->callback_drained > 1 || state->publication_pending > 1)
		return 0;
	if (state->publication_pending != !!state->pending_len)
		return 0;
	if (state->stage == VRNG_DRIVER_LIVE && state->callback_drained)
		return 0;
	if (state->stage >= VRNG_DRIVER_DRAINED &&
	    (!state->callback_drained || state->registered ||
	     state->publication_pending))
		return 0;
	if (state->stage == VRNG_DRIVER_DEAD && state->external_avail)
		return 0;
	return 1;
}

int vrng_driver_c_step(struct vrng_driver_state *state, u32 event, u32 value,
		       struct vrng_driver_outcome *outcome)
{
	if (outcome)
		memset(outcome, 0, sizeof(*outcome));
	if (!state || !outcome)
		return -EINVAL;
	if (event == VRNG_DRIVER_INIT) {
		*state = (struct vrng_driver_state) {
			.abi_version = VRNG_DRIVER_ABI_VERSION,
			.stage = VRNG_DRIVER_LIVE,
		};
		return 0;
	}
	if (!vrng_driver_c_valid(state))
		return -EINVAL;

	switch (event) {
	case VRNG_DRIVER_REGISTER:
		if (state->stage != VRNG_DRIVER_LIVE)
			return -ENODEV;
		if (state->registered)
			return -EALREADY;
		state->registered = !!value;
		return 0;
	case VRNG_DRIVER_CALLBACK_COMPLETE:
		if ((state->stage != VRNG_DRIVER_LIVE &&
		     state->stage != VRNG_DRIVER_REMOVING) ||
		    state->callback_drained || !value)
			return -EINVAL;
		state->publication_pending = 1;
		state->pending_len = value;
		return 0;
	case VRNG_DRIVER_PUBLISH:
		if (!state->publication_pending || state->callback_drained)
			return -EINVAL;
		state->external_avail = state->pending_len;
		state->publication_pending = 0;
		state->pending_len = 0;
		return 0;
	case VRNG_DRIVER_BEGIN_REMOVE:
		if (state->stage != VRNG_DRIVER_LIVE)
			return -EALREADY;
		state->stage = VRNG_DRIVER_REMOVING;
		outcome->unregister_required = state->registered;
		state->registered = 0;
		return 0;
	case VRNG_DRIVER_DRAIN:
		if (state->stage != VRNG_DRIVER_REMOVING)
			return -EINVAL;
		state->stage = VRNG_DRIVER_DRAINED;
		state->callback_drained = 1;
		state->publication_pending = 0;
		state->pending_len = 0;
		return 0;
	case VRNG_DRIVER_FINAL_CLEAR:
		if (state->stage != VRNG_DRIVER_DRAINED)
			return -EBUSY;
		state->external_avail = 0;
		return 0;
	case VRNG_DRIVER_FINISH_REMOVE:
		if (state->stage != VRNG_DRIVER_DRAINED ||
		    state->external_avail || state->publication_pending)
			return -EBUSY;
		state->stage = VRNG_DRIVER_DEAD;
		return 0;
	default:
		return -EINVAL;
	}
}
