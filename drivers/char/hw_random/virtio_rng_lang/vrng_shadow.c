// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "vrng_shadow.h"

static atomic64_t vrng_shadow_epoch = ATOMIC64_INIT(0);

bool vrng_shadow_control_valid(int result, int spec_result, u32 output,
			       u32 spec_output,
			       const struct vrng_core_state *state,
			       const struct vrng_core_state *spec_state)
{
	return result <= 0 && result == spec_result && output == spec_output &&
	       !memcmp(state, spec_state, sizeof(*state));
}

static bool vrng_shadow_states_differ(const struct vrng_shadow *shadow)
{
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	if (memcmp(&shadow->c_state, &shadow->rust_state,
		   sizeof(shadow->c_state)))
		return true;
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	if (memcmp(&shadow->c_state, &shadow->mc_state,
		   sizeof(shadow->c_state)))
		return true;
#endif
	return false;
}

static void vrng_shadow_record(struct vrng_shadow *shadow, u32 event,
			       int c_result, int rust_result, int mc_result,
			       int spec_result, u32 c_output, u32 rust_output,
			       u32 mc_output, u32 spec_output,
			       bool bytes_differ, bool oracle_differ)
{
	bool mismatch = bytes_differ || oracle_differ ||
			vrng_shadow_states_differ(shadow);

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	mismatch |= c_result != rust_result || c_output != rust_output;
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	mismatch |= c_result != mc_result || c_output != mc_output;
#endif
	shadow->sequence++;
	if (!mismatch)
		return;

	shadow->mismatches++;
	shadow->last_mismatch = (struct vrng_shadow_mismatch){
		.sequence = shadow->sequence,
		.event = event,
		.c_result = c_result,
		.rust_result = rust_result,
		.mc_result = mc_result,
		.spec_result = spec_result,
		.c_output = c_output,
		.rust_output = rust_output,
		.mc_output = mc_output,
		.spec_output = spec_output,
		.c_state = shadow->c_state,
		.rust_state = shadow->rust_state,
		.mc_state = shadow->mc_state,
		.spec_state = shadow->spec_state,
	};
}

int vrng_shadow_init(struct vrng_shadow *shadow, u32 capacity)
{
	u64 epoch = atomic64_inc_return(&vrng_shadow_epoch);
	struct vrng_spec_event event = {
		.kind = VRNG_EVENT_INIT,
		.value = capacity,
		.epoch = epoch,
	};
	struct vrng_spec_outcome outcome;
	int c_result, rust_result, mc_result, spec_result;
	bool control_valid;

	memset(shadow, 0, sizeof(*shadow));
	spin_lock_init(&shadow->lock);
	if (capacity > VRNG_SHADOW_MAX_COPY)
		return -E2BIG;

	c_result = vrng_core_c_init(&shadow->c_state, capacity, epoch);
	spec_result = vrng_spec_step(&shadow->spec_state, &event, NULL, NULL,
				     &outcome);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	rust_result = vrng_core_rust_init(&shadow->rust_state, capacity, epoch);
#else
	rust_result = c_result;
	shadow->rust_state = shadow->c_state;
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	mc_result = vrng_core_mc_init(&shadow->mc_state, capacity, epoch);
#else
	mc_result = c_result;
	shadow->mc_state = shadow->c_state;
#endif
	control_valid = vrng_shadow_control_valid(c_result, spec_result, 0, 0,
						  &shadow->c_state,
						  &shadow->spec_state);
	shadow->generation = epoch;
	shadow->active = control_valid && !c_result;
	vrng_shadow_record(shadow, VRNG_SHADOW_INIT, c_result, rust_result,
			   mc_result, spec_result, 0, 0, 0, 0, false,
			   !control_valid);
	return control_valid ? c_result : -EPROTO;
}

int vrng_shadow_begin_submit(struct vrng_shadow *shadow, u64 *generation)
{
	struct vrng_spec_event event = { .kind = VRNG_EVENT_BEGIN_SUBMIT };
	struct vrng_spec_outcome outcome;
	unsigned long flags;
	u64 c_generation = 0, rust_generation = 0, mc_generation = 0;
	int c_result, rust_result, mc_result, spec_result;
	bool control_valid;

	if (generation)
		*generation = 0;

	spin_lock_irqsave(&shadow->lock, flags);
	if (!shadow->active) {
		c_result = -ENODEV;
		goto unlock;
	}
	c_result = vrng_core_c_begin_submit(&shadow->c_state, &c_generation);
	spec_result = vrng_spec_step(&shadow->spec_state, &event, NULL, NULL,
				     &outcome);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	rust_result = vrng_core_rust_begin_submit(&shadow->rust_state,
						  &rust_generation);
#else
	rust_result = c_result;
	rust_generation = c_generation;
	shadow->rust_state = shadow->c_state;
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	mc_result =
		vrng_core_mc_begin_submit(&shadow->mc_state, &mc_generation);
#else
	mc_result = c_result;
	mc_generation = c_generation;
	shadow->mc_state = shadow->c_state;
#endif
	control_valid = vrng_shadow_control_valid(c_result, spec_result,
						  (u32)c_generation,
						  (u32)outcome.generation,
						  &shadow->c_state,
						  &shadow->spec_state) &&
			c_generation == outcome.generation;
	if (control_valid && !c_result)
		shadow->generation = c_generation;
	if (control_valid && generation)
		*generation = c_generation;
	vrng_shadow_record(shadow, VRNG_SHADOW_BEGIN_SUBMIT, c_result,
			   rust_result, mc_result, spec_result,
			   (u32)c_generation, (u32)rust_generation,
			   (u32)mc_generation, (u32)outcome.generation,
			   c_generation != rust_generation ||
				   c_generation != mc_generation,
			   !control_valid);
	if (!control_valid)
		c_result = -EPROTO;
unlock:
	spin_unlock_irqrestore(&shadow->lock, flags);
	return c_result;
}

int vrng_shadow_abort_submit(struct vrng_shadow *shadow, u64 generation)
{
	struct vrng_spec_event event = {
		.kind = VRNG_EVENT_ABORT_SUBMIT,
		.generation = generation,
	};
	struct vrng_spec_outcome outcome;
	unsigned long flags;
	int c_result, rust_result, mc_result, spec_result;
	bool control_valid;

	spin_lock_irqsave(&shadow->lock, flags);
	if (!shadow->active) {
		c_result = -ENODEV;
		goto unlock;
	}
	c_result = vrng_core_c_abort_submit(&shadow->c_state, generation);
	spec_result = vrng_spec_step(&shadow->spec_state, &event, NULL, NULL,
				     &outcome);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	rust_result =
		vrng_core_rust_abort_submit(&shadow->rust_state, generation);
#else
	rust_result = c_result;
	shadow->rust_state = shadow->c_state;
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	mc_result = vrng_core_mc_abort_submit(&shadow->mc_state, generation);
#else
	mc_result = c_result;
	shadow->mc_state = shadow->c_state;
#endif
	control_valid = vrng_shadow_control_valid(c_result, spec_result, 0, 0,
						  &shadow->c_state,
						  &shadow->spec_state);
	vrng_shadow_record(shadow, VRNG_SHADOW_ABORT_SUBMIT, c_result,
			   rust_result, mc_result, spec_result, 0, 0, 0, 0,
			   false, !control_valid);
	if (!control_valid)
		c_result = -EPROTO;
unlock:
	spin_unlock_irqrestore(&shadow->lock, flags);
	return c_result;
}

int vrng_shadow_recover_consumed(struct vrng_shadow *shadow)
{
	struct vrng_spec_event event = { .kind = VRNG_EVENT_ABORT_SUBMIT };
	struct vrng_spec_outcome outcome;
	unsigned long flags;
	int c_result, rust_result, mc_result, spec_result;
	u64 generation;
	bool control_valid;

	spin_lock_irqsave(&shadow->lock, flags);
	if (!shadow->active) {
		c_result = -ENODEV;
		goto unlock;
	}
	generation = shadow->generation;
	event.generation = generation;
	c_result = vrng_core_c_abort_submit(&shadow->c_state, generation);
	spec_result = vrng_spec_step(&shadow->spec_state, &event, NULL, NULL,
				     &outcome);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	rust_result =
		vrng_core_rust_abort_submit(&shadow->rust_state, generation);
#else
	rust_result = c_result;
	shadow->rust_state = shadow->c_state;
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	mc_result = vrng_core_mc_abort_submit(&shadow->mc_state, generation);
#else
	mc_result = c_result;
	shadow->mc_state = shadow->c_state;
#endif
	control_valid = vrng_shadow_control_valid(c_result, spec_result, 0, 0,
						  &shadow->c_state,
						  &shadow->spec_state);
	vrng_shadow_record(shadow, VRNG_SHADOW_ABORT_SUBMIT, c_result,
			   rust_result, mc_result, spec_result, 0, 0, 0, 0,
			   false, !control_valid);
	if (!control_valid)
		c_result = -EPROTO;
unlock:
	spin_unlock_irqrestore(&shadow->lock, flags);
	return c_result;
}

int vrng_shadow_complete(struct vrng_shadow *shadow, u64 generation,
			 u32 produced, u32 *need_resubmit)
{
	struct vrng_spec_event event = {
		.kind = VRNG_EVENT_COMPLETE,
		.value = produced,
		.generation = generation,
	};
	struct vrng_spec_outcome outcome;
	unsigned long flags;
	u32 c_resubmit = 0, rust_resubmit = 0, mc_resubmit = 0;
	int c_result, rust_result, mc_result, spec_result;
	bool control_valid;

	if (need_resubmit)
		*need_resubmit = 0;

	spin_lock_irqsave(&shadow->lock, flags);
	if (!shadow->active) {
		c_result = -ENODEV;
		goto unlock;
	}
	c_result = vrng_core_c_complete(&shadow->c_state, generation, produced,
					&c_resubmit);
	spec_result = vrng_spec_step(&shadow->spec_state, &event, NULL, NULL,
				     &outcome);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	rust_result = vrng_core_rust_complete(&shadow->rust_state, generation,
					      produced, &rust_resubmit);
#else
	rust_result = c_result;
	rust_resubmit = c_resubmit;
	shadow->rust_state = shadow->c_state;
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	mc_result = vrng_core_mc_complete(&shadow->mc_state, generation,
					  produced, &mc_resubmit);
#else
	mc_result = c_result;
	mc_resubmit = c_resubmit;
	shadow->mc_state = shadow->c_state;
#endif
	control_valid = vrng_shadow_control_valid(c_result, spec_result,
						  c_resubmit,
						  outcome.need_resubmit,
						  &shadow->c_state,
						  &shadow->spec_state);
	vrng_shadow_record(shadow, VRNG_SHADOW_COMPLETE, c_result, rust_result,
			   mc_result, spec_result, c_resubmit, rust_resubmit,
			   mc_resubmit, outcome.need_resubmit, false,
			   !control_valid);
	if (!control_valid) {
		c_result = -EPROTO;
		goto unlock;
	}
	if (need_resubmit)
		*need_resubmit = c_resubmit;
unlock:
	spin_unlock_irqrestore(&shadow->lock, flags);
	return c_result;
}

bool vrng_shadow_copy_output_valid(int result, u32 requested, u32 available,
				   u32 copied, u32 need_resubmit)
{
	u32 expected;

	if (result > 0 || need_resubmit > 1)
		return false;
	if (result < 0)
		return copied == 0 && need_resubmit == 0;

	expected = min(requested, available);
	if (copied != expected || copied > VRNG_SHADOW_MAX_COPY)
		return false;

	return need_resubmit == (expected && expected == available);
}

int vrng_shadow_copy(struct vrng_shadow *shadow, const u8 *dma_buffer,
		     u8 *destination, u32 requested, u32 *copied,
		     u32 *need_resubmit)
{
	u8 c_buffer[VRNG_SHADOW_MAX_COPY];
	u8 rust_buffer[VRNG_SHADOW_MAX_COPY];
	u8 mc_buffer[VRNG_SHADOW_MAX_COPY];
	u8 spec_buffer[VRNG_SHADOW_MAX_COPY];
	struct vrng_spec_event event = {
		.kind = VRNG_EVENT_COPY,
		.value = requested,
	};
	struct vrng_spec_outcome outcome;
	unsigned long flags;
	u32 c_copied = 0, rust_copied = 0, mc_copied = 0;
	u32 c_resubmit = 0, rust_resubmit = 0, mc_resubmit = 0;
	u32 pre_available;
	int c_result, rust_result, mc_result, spec_result;
	bool bytes_differ = false, c_output_valid, control_valid;

	if (copied)
		*copied = 0;
	if (need_resubmit)
		*need_resubmit = 0;

	spin_lock_irqsave(&shadow->lock, flags);
	if (!shadow->active) {
		c_result = -ENODEV;
		goto unlock;
	}
	pre_available = shadow->c_state.data_avail;
	memset(c_buffer, 0xa5, sizeof(c_buffer));
	memset(rust_buffer, 0xa5, sizeof(rust_buffer));
	memset(mc_buffer, 0xa5, sizeof(mc_buffer));
	memset(spec_buffer, 0xa5, sizeof(spec_buffer));
	c_result = vrng_core_c_copy(&shadow->c_state, dma_buffer, c_buffer,
				    requested, &c_copied, &c_resubmit);
	spec_result = vrng_spec_step(&shadow->spec_state, &event, dma_buffer,
				     spec_buffer, &outcome);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	rust_result = vrng_core_rust_copy(&shadow->rust_state, dma_buffer,
					  rust_buffer, requested, &rust_copied,
					  &rust_resubmit);
	bytes_differ |= c_copied != rust_copied;
	bytes_differ |= memcmp(c_buffer, rust_buffer, sizeof(c_buffer));
#else
	rust_result = c_result;
	rust_copied = c_copied;
	rust_resubmit = c_resubmit;
	shadow->rust_state = shadow->c_state;
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	mc_result = vrng_core_mc_copy(&shadow->mc_state, dma_buffer, mc_buffer,
				      requested, &mc_copied, &mc_resubmit);
	bytes_differ |= c_copied != mc_copied;
	bytes_differ |= memcmp(c_buffer, mc_buffer, sizeof(c_buffer));
#else
	mc_result = c_result;
	mc_copied = c_copied;
	mc_resubmit = c_resubmit;
	shadow->mc_state = shadow->c_state;
#endif
	bytes_differ |= c_resubmit != rust_resubmit ||
			c_resubmit != mc_resubmit;
	bytes_differ |=
		c_copied > sizeof(c_buffer) ||
		memchr_inv(c_buffer + min_t(u32, c_copied, sizeof(c_buffer)),
			   0xa5,
			   sizeof(c_buffer) -
				   min_t(u32, c_copied, sizeof(c_buffer)));
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	bytes_differ |=
		rust_copied > sizeof(rust_buffer) ||
		memchr_inv(rust_buffer +
				   min_t(u32, rust_copied, sizeof(rust_buffer)),
			   0xa5,
			   sizeof(rust_buffer) - min_t(u32, rust_copied,
						       sizeof(rust_buffer)));
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	bytes_differ |=
		mc_copied > sizeof(mc_buffer) ||
		memchr_inv(mc_buffer + min_t(u32, mc_copied, sizeof(mc_buffer)),
			   0xa5,
			   sizeof(mc_buffer) -
				   min_t(u32, mc_copied, sizeof(mc_buffer)));
#endif
	c_output_valid = vrng_shadow_copy_output_valid(c_result, requested,
						       pre_available, c_copied,
						       c_resubmit);
	control_valid = c_output_valid &&
			vrng_shadow_control_valid(c_result, spec_result,
						  c_copied, outcome.copied,
						  &shadow->c_state,
						  &shadow->spec_state) &&
			c_resubmit == outcome.need_resubmit &&
			!memcmp(c_buffer, spec_buffer, sizeof(c_buffer));
	vrng_shadow_record(shadow, VRNG_SHADOW_COPY, c_result, rust_result,
			   mc_result, spec_result, c_copied, rust_copied,
			   mc_copied, outcome.copied, bytes_differ,
			   !control_valid);
	if (!control_valid) {
		c_result = -EPROTO;
		goto unlock;
	}
	if (!c_result && c_copied)
		memcpy(destination, c_buffer, c_copied);
	if (copied)
		*copied = c_copied;
	if (need_resubmit)
		*need_resubmit = c_resubmit;
unlock:
	spin_unlock_irqrestore(&shadow->lock, flags);
	return c_result;
}

int vrng_shadow_begin_remove(struct vrng_shadow *shadow)
{
	struct vrng_spec_event event = { .kind = VRNG_EVENT_BEGIN_REMOVE };
	struct vrng_spec_outcome outcome;
	unsigned long flags;
	int c_result, rust_result, mc_result, spec_result;
	bool control_valid;

	spin_lock_irqsave(&shadow->lock, flags);
	if (!shadow->active) {
		c_result = -ENODEV;
		goto unlock;
	}
	c_result = vrng_core_c_begin_remove(&shadow->c_state);
	spec_result = vrng_spec_step(&shadow->spec_state, &event, NULL, NULL,
				     &outcome);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	rust_result = vrng_core_rust_begin_remove(&shadow->rust_state);
#else
	rust_result = c_result;
	shadow->rust_state = shadow->c_state;
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	mc_result = vrng_core_mc_begin_remove(&shadow->mc_state);
#else
	mc_result = c_result;
	shadow->mc_state = shadow->c_state;
#endif
	control_valid = vrng_shadow_control_valid(c_result, spec_result, 0, 0,
						  &shadow->c_state,
						  &shadow->spec_state);
	vrng_shadow_record(shadow, VRNG_SHADOW_BEGIN_REMOVE, c_result,
			   rust_result, mc_result, spec_result, 0, 0, 0, 0,
			   false, !control_valid);
	if (!control_valid)
		c_result = -EPROTO;
unlock:
	spin_unlock_irqrestore(&shadow->lock, flags);
	return c_result;
}

int vrng_shadow_finish_remove(struct vrng_shadow *shadow)
{
	struct vrng_spec_event event = { .kind = VRNG_EVENT_FINISH_REMOVE };
	struct vrng_spec_outcome outcome;
	unsigned long flags;
	int c_result, rust_result, mc_result, spec_result;
	bool control_valid;

	spin_lock_irqsave(&shadow->lock, flags);
	if (!shadow->active) {
		c_result = -ENODEV;
		goto unlock;
	}
	c_result = vrng_core_c_finish_remove(&shadow->c_state);
	spec_result = vrng_spec_step(&shadow->spec_state, &event, NULL, NULL,
				     &outcome);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	rust_result = vrng_core_rust_finish_remove(&shadow->rust_state);
#else
	rust_result = c_result;
	shadow->rust_state = shadow->c_state;
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	mc_result = vrng_core_mc_finish_remove(&shadow->mc_state);
#else
	mc_result = c_result;
	shadow->mc_state = shadow->c_state;
#endif
	control_valid = vrng_shadow_control_valid(c_result, spec_result, 0, 0,
						  &shadow->c_state,
						  &shadow->spec_state);
	vrng_shadow_record(shadow, VRNG_SHADOW_FINISH_REMOVE, c_result,
			   rust_result, mc_result, spec_result, 0, 0, 0, 0,
			   false, !control_valid);
	shadow->active = false;
	if (!control_valid)
		c_result = -EPROTO;
unlock:
	spin_unlock_irqrestore(&shadow->lock, flags);
	return c_result;
}

void vrng_shadow_snapshot(struct vrng_shadow *shadow, u64 *events,
			  u64 *mismatches, struct vrng_shadow_mismatch *last)
{
	unsigned long flags;

	spin_lock_irqsave(&shadow->lock, flags);
	*events = shadow->sequence;
	*mismatches = shadow->mismatches;
	*last = shadow->last_mismatch;
	spin_unlock_irqrestore(&shadow->lock, flags);
}
