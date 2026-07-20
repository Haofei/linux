// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/errno.h>
#include <linux/minmax.h>
#include <linux/nospec.h>
#include <linux/overflow.h>
#include <linux/string.h>

#include "vrng_core_abi.h"

u32 vrng_core_index_nospec(u32 index, u32 size)
{
	return array_index_nospec(index, size);
}

static void vrng_core_c_empty(struct vrng_core_state *state)
{
	state->phase = VRNG_EMPTY;
	state->data_idx = 0;
	state->data_avail = 0;
}

int vrng_core_c_validate(const struct vrng_core_state *state)
{
	u32 end;

	if (!state || state->abi_version != VRNG_CORE_ABI_VERSION)
		return -EINVAL;
	if (state->lifecycle > VRNG_DEAD || state->phase > VRNG_READY)
		return -EINVAL;
	if (!state->capacity || state->data_idx > state->capacity ||
	    state->data_avail > state->capacity)
		return -EINVAL;
	if (check_add_overflow(state->data_idx, state->data_avail, &end) ||
	    end > state->capacity)
		return -EINVAL;

	switch (state->phase) {
	case VRNG_EMPTY:
	case VRNG_DEVICE_OWNED:
		if (state->data_idx || state->data_avail)
			return -EINVAL;
		break;
	case VRNG_READY:
		if (state->lifecycle != VRNG_ACTIVE || !state->data_avail)
			return -EINVAL;
		break;
	}

	if (state->lifecycle == VRNG_DEAD && state->phase != VRNG_EMPTY)
		return -EINVAL;
	if (state->lifecycle == VRNG_QUIESCING && state->phase == VRNG_READY)
		return -EINVAL;

	return 0;
}

int vrng_core_c_init(struct vrng_core_state *state, u32 capacity, u64 epoch)
{
	if (!state || !capacity)
		return -EINVAL;

	memset(state, 0, sizeof(*state));
	state->abi_version = VRNG_CORE_ABI_VERSION;
	state->lifecycle = VRNG_ACTIVE;
	state->phase = VRNG_EMPTY;
	state->capacity = capacity;
	state->epoch = epoch;
	state->generation = epoch;

	return 0;
}

int vrng_core_c_begin_submit(struct vrng_core_state *state, u64 *generation)
{
	u64 next;
	int ret;

	if (!generation)
		return -EINVAL;
	*generation = 0;

	ret = vrng_core_c_validate(state);
	if (ret)
		return ret;
	if (state->lifecycle != VRNG_ACTIVE)
		return -ENODEV;
	if (state->phase != VRNG_EMPTY)
		return -EBUSY;
	if (check_add_overflow(state->generation, 1ULL, &next))
		return -EOVERFLOW;

	state->generation = next;
	state->phase = VRNG_DEVICE_OWNED;
	*generation = next;
	return 0;
}

int vrng_core_c_abort_submit(struct vrng_core_state *state, u64 generation)
{
	int ret;

	ret = vrng_core_c_validate(state);
	if (ret)
		return ret;
	if (state->lifecycle == VRNG_DEAD)
		return -ENODEV;
	if (state->phase != VRNG_DEVICE_OWNED)
		return generation == state->generation ? -EALREADY : -ESTALE;
	if (generation != state->generation)
		return -ESTALE;

	vrng_core_c_empty(state);
	return 0;
}

int vrng_core_c_complete(struct vrng_core_state *state, u64 generation,
			 u32 produced, u32 *need_resubmit)
{
	int ret;

	if (!need_resubmit)
		return -EINVAL;
	*need_resubmit = 0;

	ret = vrng_core_c_validate(state);
	if (ret)
		return ret;
	if (state->lifecycle == VRNG_DEAD)
		return -ENODEV;
	if (state->phase != VRNG_DEVICE_OWNED)
		return generation == state->generation ? -EALREADY : -ESTALE;
	if (generation != state->generation)
		return -ESTALE;

	if (state->lifecycle == VRNG_QUIESCING) {
		vrng_core_c_empty(state);
		return -ENODEV;
	}

	if (!produced) {
		vrng_core_c_empty(state);
		*need_resubmit = 1;
		return -ENODATA;
	}
	if (produced > state->capacity) {
		vrng_core_c_empty(state);
		*need_resubmit = 1;
		return -EOVERFLOW;
	}

	state->phase = VRNG_READY;
	state->data_idx = 0;
	state->data_avail = produced;
	return 0;
}

int vrng_core_c_copy(struct vrng_core_state *state, const u8 *dma_buffer,
		     u8 *destination, u32 requested, u32 *copied,
		     u32 *need_resubmit)
{
	u32 amount, idx, next_idx, next_avail;
	int ret;

	if (copied)
		*copied = 0;
	if (need_resubmit)
		*need_resubmit = 0;
	if (!copied || !need_resubmit)
		return -EINVAL;

	ret = vrng_core_c_validate(state);
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
	if (check_add_overflow(state->data_idx, amount, &next_idx) ||
	    check_sub_overflow(state->data_avail, amount, &next_avail) ||
	    next_idx > state->capacity)
		return -EOVERFLOW;

	idx = vrng_core_index_nospec(state->data_idx, state->capacity);
	memcpy(destination, dma_buffer + idx, amount);
	*copied = amount;

	if (!next_avail) {
		vrng_core_c_empty(state);
		*need_resubmit = 1;
	} else {
		state->data_idx = next_idx;
		state->data_avail = next_avail;
	}

	return 0;
}

int vrng_core_c_begin_remove(struct vrng_core_state *state)
{
	int ret;

	ret = vrng_core_c_validate(state);
	if (ret)
		return ret;
	if (state->lifecycle != VRNG_ACTIVE)
		return -EALREADY;

	if (state->phase == VRNG_READY)
		vrng_core_c_empty(state);
	state->lifecycle = VRNG_QUIESCING;
	return 0;
}

int vrng_core_c_finish_remove(struct vrng_core_state *state)
{
	int ret;

	ret = vrng_core_c_validate(state);
	if (ret)
		return ret;
	if (state->lifecycle == VRNG_DEAD)
		return -EALREADY;
	if (state->lifecycle != VRNG_QUIESCING)
		return -EINVAL;

	/* The glue guarantees that reset and del_vqs drained DeviceOwned. */
	vrng_core_c_empty(state);
	state->lifecycle = VRNG_DEAD;
	return 0;
}
