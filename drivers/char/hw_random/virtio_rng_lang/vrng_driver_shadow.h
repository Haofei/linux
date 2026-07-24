/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _VRNG_DRIVER_SHADOW_H
#define _VRNG_DRIVER_SHADOW_H

#include <linux/spinlock.h>

#include "vrng_driver_abi.h"

struct vrng_driver_shadow {
	/* Serializes all candidate and specification transitions. */
	spinlock_t lock;
	struct vrng_driver_state c_state;
	struct vrng_driver_state rust_raw_state;
	struct vrng_driver_state rust_safe_state;
	struct vrng_driver_state mc_raw_state;
	struct vrng_driver_state mc_contract_state;
	struct vrng_driver_state spec_state;
	u64 events;
	u64 mismatches;
	bool active;
};

int vrng_driver_shadow_init(struct vrng_driver_shadow *shadow);
int vrng_driver_shadow_register(struct vrng_driver_shadow *shadow, bool success);
int vrng_driver_shadow_complete(struct vrng_driver_shadow *shadow, u32 length);
int vrng_driver_shadow_publish(struct vrng_driver_shadow *shadow);
int vrng_driver_shadow_begin_remove(struct vrng_driver_shadow *shadow,
				    bool *unregister_required);
int vrng_driver_shadow_drain(struct vrng_driver_shadow *shadow);
int vrng_driver_shadow_final_clear(struct vrng_driver_shadow *shadow);
int vrng_driver_shadow_finish_remove(struct vrng_driver_shadow *shadow);
void vrng_driver_shadow_snapshot(struct vrng_driver_shadow *shadow, u64 *events,
				 u64 *mismatches,
				 struct vrng_driver_state *selected);

#endif
