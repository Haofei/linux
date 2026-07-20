// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/errno.h>
#include <linux/minmax.h>
#include <linux/nospec.h>
#include <linux/string.h>

#include "vrng_core_spec.h"

/*
 * Executable oracle for the protocol.  This intentionally does not call the C
 * candidate functions or share their transition helpers.
 */
static int vrng_spec_well_formed(const struct vrng_core_state *state)
{
	if (!state || state->abi_version != VRNG_CORE_ABI_VERSION)
		return -EINVAL;
	if (state->lifecycle > VRNG_DEAD || state->phase > VRNG_READY)
		return -EINVAL;
	if (!state->capacity || state->data_idx > state->capacity ||
	    state->data_avail > state->capacity)
		return -EINVAL;
	if (state->data_idx > state->capacity - state->data_avail)
		return -EINVAL;
	if ((state->phase == VRNG_EMPTY || state->phase == VRNG_DEVICE_OWNED) &&
	    (state->data_idx || state->data_avail))
		return -EINVAL;
	if (state->phase == VRNG_READY &&
	    (state->lifecycle != VRNG_ACTIVE || !state->data_avail))
		return -EINVAL;
	if (state->lifecycle == VRNG_DEAD && state->phase != VRNG_EMPTY)
		return -EINVAL;
	if (state->lifecycle == VRNG_QUIESCING && state->phase == VRNG_READY)
		return -EINVAL;
	return 0;
}

static void vrng_spec_make_empty(struct vrng_core_state *state)
{
	state->phase = VRNG_EMPTY;
	state->data_idx = 0;
	state->data_avail = 0;
}

static int vrng_spec_begin_submit(struct vrng_core_state *state,
				  struct vrng_spec_outcome *outcome)
{
	int ret = vrng_spec_well_formed(state);

	if (ret)
		return ret;
	if (state->lifecycle != VRNG_ACTIVE)
		return -ENODEV;
	if (state->phase != VRNG_EMPTY)
		return -EBUSY;
	if (state->generation == U64_MAX)
		return -EOVERFLOW;

	state->generation++;
	state->phase = VRNG_DEVICE_OWNED;
	outcome->generation = state->generation;
	return 0;
}

static int vrng_spec_abort_submit(struct vrng_core_state *state, u64 generation)
{
	int ret = vrng_spec_well_formed(state);

	if (ret)
		return ret;
	if (state->lifecycle == VRNG_DEAD)
		return -ENODEV;
	if (state->phase != VRNG_DEVICE_OWNED)
		return generation == state->generation ? -EALREADY : -ESTALE;
	if (generation != state->generation)
		return -ESTALE;
	vrng_spec_make_empty(state);
	return 0;
}

static int vrng_spec_complete(struct vrng_core_state *state, u64 generation,
			      u32 produced, struct vrng_spec_outcome *outcome)
{
	int ret = vrng_spec_well_formed(state);

	if (ret)
		return ret;
	if (state->lifecycle == VRNG_DEAD)
		return -ENODEV;
	if (state->phase != VRNG_DEVICE_OWNED)
		return generation == state->generation ? -EALREADY : -ESTALE;
	if (generation != state->generation)
		return -ESTALE;
	if (state->lifecycle == VRNG_QUIESCING) {
		vrng_spec_make_empty(state);
		return -ENODEV;
	}
	if (!produced) {
		vrng_spec_make_empty(state);
		outcome->need_resubmit = 1;
		return -ENODATA;
	}
	if (produced > state->capacity) {
		vrng_spec_make_empty(state);
		outcome->need_resubmit = 1;
		return -EOVERFLOW;
	}

	state->phase = VRNG_READY;
	state->data_idx = 0;
	state->data_avail = produced;
	return 0;
}

static int vrng_spec_copy(struct vrng_core_state *state, u32 requested,
			  const u8 *dma_buffer, u8 *destination,
			  struct vrng_spec_outcome *outcome)
{
	u32 amount, index;
	int ret = vrng_spec_well_formed(state);

	if (ret)
		return ret;
	if (!dma_buffer || !destination)
		return -EINVAL;
	if (state->lifecycle != VRNG_ACTIVE)
		return -ENODEV;
	if (state->phase == VRNG_EMPTY)
		return -EAGAIN;
	if (state->phase == VRNG_DEVICE_OWNED)
		return -EBUSY;
	if (!requested)
		return 0;

	amount = min(requested, state->data_avail);
	if (amount > state->capacity - state->data_idx ||
	    amount > state->data_avail)
		return -EOVERFLOW;

	index = array_index_nospec(state->data_idx, state->capacity);
	memcpy(destination, dma_buffer + index, amount);
	outcome->copied = amount;
	state->data_idx += amount;
	state->data_avail -= amount;
	if (!state->data_avail) {
		vrng_spec_make_empty(state);
		outcome->need_resubmit = 1;
	}
	return 0;
}

int vrng_spec_step(struct vrng_core_state *state,
		   const struct vrng_spec_event *event, const u8 *dma_buffer,
		   u8 *destination, struct vrng_spec_outcome *outcome)
{
	int ret;

	if (!event || !outcome)
		return -EINVAL;
	memset(outcome, 0, sizeof(*outcome));

	switch (event->kind) {
	case VRNG_EVENT_INIT:
		if (!state || !event->value) {
			ret = -EINVAL;
			break;
		}
		memset(state, 0, sizeof(*state));
		state->abi_version = VRNG_CORE_ABI_VERSION;
		state->lifecycle = VRNG_ACTIVE;
		state->phase = VRNG_EMPTY;
		state->capacity = event->value;
		state->epoch = event->epoch;
		state->generation = event->epoch;
		ret = 0;
		break;
	case VRNG_EVENT_BEGIN_SUBMIT:
		ret = vrng_spec_begin_submit(state, outcome);
		break;
	case VRNG_EVENT_ABORT_SUBMIT:
		ret = vrng_spec_abort_submit(state, event->generation);
		break;
	case VRNG_EVENT_COMPLETE:
		ret = vrng_spec_complete(state, event->generation, event->value,
					 outcome);
		break;
	case VRNG_EVENT_COPY:
		ret = vrng_spec_copy(state, event->value, dma_buffer,
				     destination, outcome);
		break;
	case VRNG_EVENT_BEGIN_REMOVE:
		ret = vrng_spec_well_formed(state);
		if (ret)
			break;
		if (state->lifecycle != VRNG_ACTIVE) {
			ret = -EALREADY;
			break;
		}
		if (state->phase == VRNG_READY)
			vrng_spec_make_empty(state);
		state->lifecycle = VRNG_QUIESCING;
		ret = 0;
		break;
	case VRNG_EVENT_FINISH_REMOVE:
		ret = vrng_spec_well_formed(state);
		if (ret)
			break;
		if (state->lifecycle == VRNG_DEAD) {
			ret = -EALREADY;
			break;
		}
		if (state->lifecycle != VRNG_QUIESCING) {
			ret = -EINVAL;
			break;
		}
		vrng_spec_make_empty(state);
		state->lifecycle = VRNG_DEAD;
		ret = 0;
		break;
	case VRNG_EVENT_VALIDATE:
		ret = vrng_spec_well_formed(state);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	outcome->result = ret;
	return ret;
}
