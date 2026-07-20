// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "vrng_shadow.h"

static atomic64_t vrng_shadow_epoch = ATOMIC64_INIT(0);

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
			       u32 c_output, u32 rust_output, u32 mc_output,
			       bool bytes_differ)
{
	bool mismatch = bytes_differ || vrng_shadow_states_differ(shadow);

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
	shadow->last_mismatch = (struct vrng_shadow_mismatch) {
		.sequence = shadow->sequence,
		.event = event,
		.c_result = c_result,
		.rust_result = rust_result,
		.mc_result = mc_result,
		.c_output = c_output,
		.rust_output = rust_output,
		.mc_output = mc_output,
		.c_state = shadow->c_state,
		.rust_state = shadow->rust_state,
		.mc_state = shadow->mc_state,
	};
}

int vrng_shadow_init(struct vrng_shadow *shadow, u32 capacity)
{
	u64 epoch = atomic64_inc_return(&vrng_shadow_epoch);
	int c_result, rust_result, mc_result;

	memset(shadow, 0, sizeof(*shadow));
	spin_lock_init(&shadow->lock);
	if (capacity > VRNG_SHADOW_MAX_COPY)
		return -E2BIG;

	c_result = vrng_core_c_init(&shadow->c_state, capacity, epoch);
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
	shadow->generation = epoch;
	shadow->active = !c_result;
	vrng_shadow_record(shadow, VRNG_SHADOW_INIT, c_result, rust_result,
			   mc_result, 0, 0, 0, false);
	return c_result;
}

int vrng_shadow_begin_submit(struct vrng_shadow *shadow, u64 *generation)
{
	unsigned long flags;
	u64 c_generation = 0, rust_generation = 0, mc_generation = 0;
	int c_result, rust_result, mc_result;

	spin_lock_irqsave(&shadow->lock, flags);
	if (!shadow->active) {
		c_result = -ENODEV;
		goto unlock;
	}
	c_result = vrng_core_c_begin_submit(&shadow->c_state, &c_generation);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	rust_result = vrng_core_rust_begin_submit(&shadow->rust_state,
						  &rust_generation);
#else
	rust_result = c_result;
	rust_generation = c_generation;
	shadow->rust_state = shadow->c_state;
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	mc_result = vrng_core_mc_begin_submit(&shadow->mc_state,
					      &mc_generation);
#else
	mc_result = c_result;
	mc_generation = c_generation;
	shadow->mc_state = shadow->c_state;
#endif
	if (!c_result)
		shadow->generation = c_generation;
	if (generation)
		*generation = c_generation;
	vrng_shadow_record(shadow, VRNG_SHADOW_BEGIN_SUBMIT, c_result,
			   rust_result, mc_result, (u32)c_generation,
			   (u32)rust_generation, (u32)mc_generation,
			   c_generation != rust_generation ||
			   c_generation != mc_generation);
unlock:
	spin_unlock_irqrestore(&shadow->lock, flags);
	return c_result;
}

int vrng_shadow_abort_submit(struct vrng_shadow *shadow, u64 generation)
{
	unsigned long flags;
	int c_result, rust_result, mc_result;

	spin_lock_irqsave(&shadow->lock, flags);
	if (!shadow->active) {
		c_result = -ENODEV;
		goto unlock;
	}
	c_result = vrng_core_c_abort_submit(&shadow->c_state,
					    generation);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	rust_result = vrng_core_rust_abort_submit(&shadow->rust_state,
						  generation);
#else
	rust_result = c_result;
	shadow->rust_state = shadow->c_state;
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	mc_result = vrng_core_mc_abort_submit(&shadow->mc_state,
					      generation);
#else
	mc_result = c_result;
	shadow->mc_state = shadow->c_state;
#endif
	vrng_shadow_record(shadow, VRNG_SHADOW_ABORT_SUBMIT, c_result,
			   rust_result, mc_result, 0, 0, 0, false);
unlock:
	spin_unlock_irqrestore(&shadow->lock, flags);
	return c_result;
}

int vrng_shadow_recover_consumed(struct vrng_shadow *shadow)
{
	unsigned long flags;
	int c_result, rust_result, mc_result;
	u64 generation;

	spin_lock_irqsave(&shadow->lock, flags);
	if (!shadow->active) {
		c_result = -ENODEV;
		goto unlock;
	}
	generation = shadow->generation;
	c_result = vrng_core_c_abort_submit(&shadow->c_state, generation);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	rust_result = vrng_core_rust_abort_submit(&shadow->rust_state, generation);
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
	vrng_shadow_record(shadow, VRNG_SHADOW_ABORT_SUBMIT, c_result,
			   rust_result, mc_result, 0, 0, 0, false);
unlock:
	spin_unlock_irqrestore(&shadow->lock, flags);
	return c_result;
}

int vrng_shadow_complete(struct vrng_shadow *shadow, u64 generation,
			 u32 produced, u32 *need_resubmit)
{
	unsigned long flags;
	u32 c_resubmit = 0, rust_resubmit = 0, mc_resubmit = 0;
	int c_result, rust_result, mc_result;

	spin_lock_irqsave(&shadow->lock, flags);
	if (!shadow->active) {
		c_result = -ENODEV;
		goto unlock;
	}
	c_result = vrng_core_c_complete(&shadow->c_state, generation,
					produced, &c_resubmit);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	rust_result = vrng_core_rust_complete(&shadow->rust_state,
					      generation, produced,
					      &rust_resubmit);
#else
	rust_result = c_result;
	rust_resubmit = c_resubmit;
	shadow->rust_state = shadow->c_state;
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	mc_result = vrng_core_mc_complete(&shadow->mc_state,
					  generation, produced,
					  &mc_resubmit);
#else
	mc_result = c_result;
	mc_resubmit = c_resubmit;
	shadow->mc_state = shadow->c_state;
#endif
	vrng_shadow_record(shadow, VRNG_SHADOW_COMPLETE, c_result, rust_result,
			   mc_result, c_resubmit, rust_resubmit, mc_resubmit,
			   false);
	if (need_resubmit)
		*need_resubmit = c_resubmit;
unlock:
	spin_unlock_irqrestore(&shadow->lock, flags);
	return c_result;
}

int vrng_shadow_copy(struct vrng_shadow *shadow, const u8 *dma_buffer,
		     u8 *destination, u32 requested, u32 *copied,
		     u32 *need_resubmit)
{
	u8 rust_buffer[VRNG_SHADOW_MAX_COPY];
	u8 mc_buffer[VRNG_SHADOW_MAX_COPY];
	unsigned long flags;
	u32 c_copied = 0, rust_copied = 0, mc_copied = 0;
	u32 c_resubmit = 0, rust_resubmit = 0, mc_resubmit = 0;
	int c_result, rust_result, mc_result;
	bool bytes_differ = false;

	spin_lock_irqsave(&shadow->lock, flags);
	if (!shadow->active) {
		c_result = -ENODEV;
		goto unlock;
	}
	memset(rust_buffer, 0xa5, sizeof(rust_buffer));
	memset(mc_buffer, 0xa5, sizeof(mc_buffer));
	c_result = vrng_core_c_copy(&shadow->c_state, dma_buffer, destination,
				    requested, &c_copied, &c_resubmit);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	rust_result = vrng_core_rust_copy(&shadow->rust_state, dma_buffer,
					  rust_buffer, requested, &rust_copied,
					  &rust_resubmit);
	bytes_differ |= c_copied != rust_copied ||
		memcmp(destination, rust_buffer, c_copied);
#else
	rust_result = c_result;
	rust_copied = c_copied;
	rust_resubmit = c_resubmit;
	shadow->rust_state = shadow->c_state;
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	mc_result = vrng_core_mc_copy(&shadow->mc_state, dma_buffer, mc_buffer,
				      requested, &mc_copied, &mc_resubmit);
	bytes_differ |= c_copied != mc_copied ||
		memcmp(destination, mc_buffer, c_copied);
#else
	mc_result = c_result;
	mc_copied = c_copied;
	mc_resubmit = c_resubmit;
	shadow->mc_state = shadow->c_state;
#endif
	bytes_differ |= c_resubmit != rust_resubmit ||
			c_resubmit != mc_resubmit;
	vrng_shadow_record(shadow, VRNG_SHADOW_COPY, c_result, rust_result,
			   mc_result, c_copied, rust_copied, mc_copied,
			   bytes_differ);
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
	unsigned long flags;
	int c_result, rust_result, mc_result;

	spin_lock_irqsave(&shadow->lock, flags);
	if (!shadow->active) {
		c_result = -ENODEV;
		goto unlock;
	}
	c_result = vrng_core_c_begin_remove(&shadow->c_state);
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
	vrng_shadow_record(shadow, VRNG_SHADOW_BEGIN_REMOVE, c_result,
			   rust_result, mc_result, 0, 0, 0, false);
unlock:
	spin_unlock_irqrestore(&shadow->lock, flags);
	return c_result;
}

int vrng_shadow_finish_remove(struct vrng_shadow *shadow)
{
	unsigned long flags;
	int c_result, rust_result, mc_result;

	spin_lock_irqsave(&shadow->lock, flags);
	if (!shadow->active) {
		c_result = -ENODEV;
		goto unlock;
	}
	c_result = vrng_core_c_finish_remove(&shadow->c_state);
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
	vrng_shadow_record(shadow, VRNG_SHADOW_FINISH_REMOVE, c_result,
			   rust_result, mc_result, 0, 0, 0, false);
	shadow->active = false;
unlock:
	spin_unlock_irqrestore(&shadow->lock, flags);
	return c_result;
}

void vrng_shadow_snapshot(struct vrng_shadow *shadow, u64 *events,
			  u64 *mismatches,
			  struct vrng_shadow_mismatch *last)
{
	unsigned long flags;

	spin_lock_irqsave(&shadow->lock, flags);
	*events = shadow->sequence;
	*mismatches = shadow->mismatches;
	*last = shadow->last_mismatch;
	spin_unlock_irqrestore(&shadow->lock, flags);
}
