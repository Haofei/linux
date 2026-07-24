// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/errno.h>
#include <linux/string.h>

#include "vrng_driver_shadow.h"

static struct vrng_driver_state *
vrng_driver_selected_state(struct vrng_driver_shadow *shadow)
{
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_CONTROL_RUST)
	return &shadow->rust_safe_state;
#elif IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_CONTROL_MC)
	return &shadow->mc_contract_state;
#else
	return &shadow->c_state;
#endif
}

static int vrng_driver_selected_result(int c_result, int rust_safe_result,
				       int mc_contract_result)
{
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_CONTROL_RUST)
	return rust_safe_result;
#elif IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_CONTROL_MC)
	return mc_contract_result;
#else
	return c_result;
#endif
}

static struct vrng_driver_outcome *
vrng_driver_selected_outcome(struct vrng_driver_outcome *c_outcome,
			     struct vrng_driver_outcome *rust_safe_outcome,
			     struct vrng_driver_outcome *mc_contract_outcome)
{
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_CONTROL_RUST)
	return rust_safe_outcome;
#elif IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_CONTROL_MC)
	return mc_contract_outcome;
#else
	return c_outcome;
#endif
}

static int vrng_driver_shadow_step_locked(struct vrng_driver_shadow *shadow,
					  u32 event, u32 value,
					  struct vrng_driver_outcome *selected)
{
	struct vrng_driver_outcome c_out = {}, rust_raw_out = {};
	struct vrng_driver_outcome rust_safe_out = {}, mc_raw_out = {};
	struct vrng_driver_outcome mc_contract_out = {}, spec_out = {};
	int c_result, rust_raw_result, rust_safe_result;
	int mc_raw_result, mc_contract_result, spec_result, selected_result;
	bool mismatch;

	c_result = vrng_driver_c_step(&shadow->c_state, event, value, &c_out);
	spec_result = vrng_driver_spec_step(&shadow->spec_state, event, value,
					    &spec_out);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	rust_raw_result = vrng_driver_rust_raw_step(&shadow->rust_raw_state,
						    event, value,
						    &rust_raw_out);
	rust_safe_result = vrng_driver_rust_safe_step(&shadow->rust_safe_state,
						      event, value,
						      &rust_safe_out);
#else
	rust_raw_result = c_result;
	rust_safe_result = c_result;
	shadow->rust_raw_state = shadow->c_state;
	shadow->rust_safe_state = shadow->c_state;
	rust_raw_out = c_out;
	rust_safe_out = c_out;
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	mc_raw_result = vrng_driver_mc_raw_step(&shadow->mc_raw_state, event,
						value, &mc_raw_out);
	mc_contract_result =
		vrng_driver_mc_contract_step(&shadow->mc_contract_state, event,
					     value, &mc_contract_out);
#else
	mc_raw_result = c_result;
	mc_contract_result = c_result;
	shadow->mc_raw_state = shadow->c_state;
	shadow->mc_contract_state = shadow->c_state;
	mc_raw_out = c_out;
	mc_contract_out = c_out;
#endif
	selected_result = vrng_driver_selected_result(c_result, rust_safe_result,
						      mc_contract_result);
	*selected = *vrng_driver_selected_outcome(&c_out, &rust_safe_out,
						  &mc_contract_out);
	mismatch = c_result != spec_result || rust_raw_result != spec_result ||
		   rust_safe_result != spec_result || mc_raw_result != spec_result ||
		   mc_contract_result != spec_result ||
		   memcmp(&c_out, &spec_out, sizeof(spec_out)) ||
		   memcmp(&rust_raw_out, &spec_out, sizeof(spec_out)) ||
		   memcmp(&rust_safe_out, &spec_out, sizeof(spec_out)) ||
		   memcmp(&mc_raw_out, &spec_out, sizeof(spec_out)) ||
		   memcmp(&mc_contract_out, &spec_out, sizeof(spec_out)) ||
		   memcmp(&shadow->c_state, &shadow->spec_state,
			  sizeof(shadow->spec_state)) ||
		   memcmp(&shadow->rust_raw_state, &shadow->spec_state,
			  sizeof(shadow->spec_state)) ||
		   memcmp(&shadow->rust_safe_state, &shadow->spec_state,
			  sizeof(shadow->spec_state)) ||
		   memcmp(&shadow->mc_raw_state, &shadow->spec_state,
			  sizeof(shadow->spec_state)) ||
		   memcmp(&shadow->mc_contract_state, &shadow->spec_state,
			  sizeof(shadow->spec_state));
	shadow->events++;
	if (mismatch) {
		shadow->mismatches++;
		return -EPROTO;
	}
	return selected_result;
}

static int vrng_driver_shadow_step(struct vrng_driver_shadow *shadow, u32 event,
				   u32 value,
				   struct vrng_driver_outcome *selected)
{
	unsigned long flags;
	int result;

	spin_lock_irqsave(&shadow->lock, flags);
	if (!shadow->active && event != VRNG_DRIVER_INIT) {
		result = -ENODEV;
		goto unlock;
	}
	result = vrng_driver_shadow_step_locked(shadow, event, value, selected);
	if (event == VRNG_DRIVER_INIT)
		shadow->active = !result;
	if (event == VRNG_DRIVER_FINISH_REMOVE && !result)
		shadow->active = false;
unlock:
	spin_unlock_irqrestore(&shadow->lock, flags);
	return result;
}

int vrng_driver_shadow_init(struct vrng_driver_shadow *shadow)
{
	struct vrng_driver_outcome ignored;

	memset(shadow, 0, sizeof(*shadow));
	spin_lock_init(&shadow->lock);
	return vrng_driver_shadow_step(shadow, VRNG_DRIVER_INIT, 0, &ignored);
}

int vrng_driver_shadow_register(struct vrng_driver_shadow *shadow, bool success)
{
	struct vrng_driver_outcome ignored;

	return vrng_driver_shadow_step(shadow, VRNG_DRIVER_REGISTER, success,
				       &ignored);
}

int vrng_driver_shadow_complete(struct vrng_driver_shadow *shadow, u32 length)
{
	struct vrng_driver_outcome ignored;

	return vrng_driver_shadow_step(shadow, VRNG_DRIVER_CALLBACK_COMPLETE,
				       length, &ignored);
}

int vrng_driver_shadow_publish(struct vrng_driver_shadow *shadow)
{
	struct vrng_driver_outcome ignored;

	return vrng_driver_shadow_step(shadow, VRNG_DRIVER_PUBLISH, 0, &ignored);
}

int vrng_driver_shadow_begin_remove(struct vrng_driver_shadow *shadow,
				    bool *unregister_required)
{
	struct vrng_driver_outcome selected;
	int result;

	if (unregister_required)
		*unregister_required = false;
	result = vrng_driver_shadow_step(shadow, VRNG_DRIVER_BEGIN_REMOVE, 0,
					 &selected);
	if (!result && unregister_required)
		*unregister_required = selected.unregister_required;
	return result;
}

int vrng_driver_shadow_drain(struct vrng_driver_shadow *shadow)
{
	struct vrng_driver_outcome ignored;

	return vrng_driver_shadow_step(shadow, VRNG_DRIVER_DRAIN, 0, &ignored);
}

int vrng_driver_shadow_final_clear(struct vrng_driver_shadow *shadow)
{
	struct vrng_driver_outcome ignored;

	return vrng_driver_shadow_step(shadow, VRNG_DRIVER_FINAL_CLEAR, 0,
				       &ignored);
}

int vrng_driver_shadow_finish_remove(struct vrng_driver_shadow *shadow)
{
	struct vrng_driver_outcome ignored;

	return vrng_driver_shadow_step(shadow, VRNG_DRIVER_FINISH_REMOVE, 0,
				       &ignored);
}

void vrng_driver_shadow_snapshot(struct vrng_driver_shadow *shadow, u64 *events,
				 u64 *mismatches,
				 struct vrng_driver_state *selected)
{
	unsigned long flags;

	spin_lock_irqsave(&shadow->lock, flags);
	if (events)
		*events = shadow->events;
	if (mismatches)
		*mismatches = shadow->mismatches;
	if (selected)
		*selected = *vrng_driver_selected_state(shadow);
	spin_unlock_irqrestore(&shadow->lock, flags);
}
