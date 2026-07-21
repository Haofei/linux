// SPDX-License-Identifier: GPL-2.0-or-later
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>

#include "../virtio_rng_internal.h"
#include "vrng_core_abi.h"
#include "vrng_core_spec.h"
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
#include "vrng_shadow.h"
#endif

#define VRNG_TEST_BUFFER_SIZE 32U
#define VRNG_BFS_MAX_STATES 1024U
#define VRNG_BFS_DEPTH 7U

struct vrng_core_ops {
	int (*init)(struct vrng_core_state *state, u32 capacity, u64 epoch);
	int (*begin_submit)(struct vrng_core_state *state, u64 *generation);
	int (*abort_submit)(struct vrng_core_state *state, u64 generation);
	int (*complete)(struct vrng_core_state *state, u64 generation,
			u32 produced, u32 *need_resubmit);
	int (*copy)(struct vrng_core_state *state, const u8 *dma_buffer,
		    u8 *destination, u32 requested, u32 *copied,
		    u32 *need_resubmit);
	int (*begin_remove)(struct vrng_core_state *state);
	int (*finish_remove)(struct vrng_core_state *state);
	int (*validate)(const struct vrng_core_state *state);
};

static const struct vrng_core_ops vrng_c_ops = {
	.init = vrng_core_c_init,
	.begin_submit = vrng_core_c_begin_submit,
	.abort_submit = vrng_core_c_abort_submit,
	.complete = vrng_core_c_complete,
	.copy = vrng_core_c_copy,
	.begin_remove = vrng_core_c_begin_remove,
	.finish_remove = vrng_core_c_finish_remove,
	.validate = vrng_core_c_validate,
};

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
static const struct vrng_core_ops vrng_rust_ops = {
	.init = vrng_core_rust_init,
	.begin_submit = vrng_core_rust_begin_submit,
	.abort_submit = vrng_core_rust_abort_submit,
	.complete = vrng_core_rust_complete,
	.copy = vrng_core_rust_copy,
	.begin_remove = vrng_core_rust_begin_remove,
	.finish_remove = vrng_core_rust_finish_remove,
	.validate = vrng_core_rust_validate,
};
#endif

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
static const struct vrng_core_ops vrng_mc_ops = {
	.init = vrng_core_mc_init,
	.begin_submit = vrng_core_mc_begin_submit,
	.abort_submit = vrng_core_mc_abort_submit,
	.complete = vrng_core_mc_complete,
	.copy = vrng_core_mc_copy,
	.begin_remove = vrng_core_mc_begin_remove,
	.finish_remove = vrng_core_mc_finish_remove,
	.validate = vrng_core_mc_validate,
};
#endif

static int vrng_run_event(const struct vrng_core_ops *ops,
			  struct vrng_core_state *state,
			  const struct vrng_spec_event *event,
			  const u8 *dma_buffer, u8 *destination,
			  struct vrng_spec_outcome *outcome)
{
	u32 copied = 0, need_resubmit = 0;
	u64 generation = 0;
	int ret;

	memset(outcome, 0, sizeof(*outcome));
	switch (event->kind) {
	case VRNG_EVENT_INIT:
		ret = ops->init(state, event->value, event->epoch);
		break;
	case VRNG_EVENT_BEGIN_SUBMIT:
		ret = ops->begin_submit(state, &generation);
		outcome->generation = generation;
		break;
	case VRNG_EVENT_ABORT_SUBMIT:
		ret = ops->abort_submit(state, event->generation);
		break;
	case VRNG_EVENT_COMPLETE:
		ret = ops->complete(state, event->generation, event->value,
				    &need_resubmit);
		outcome->need_resubmit = need_resubmit;
		break;
	case VRNG_EVENT_COPY:
		ret = ops->copy(state, dma_buffer, destination, event->value,
				&copied, &need_resubmit);
		outcome->copied = copied;
		outcome->need_resubmit = need_resubmit;
		break;
	case VRNG_EVENT_BEGIN_REMOVE:
		ret = ops->begin_remove(state);
		break;
	case VRNG_EVENT_FINISH_REMOVE:
		ret = ops->finish_remove(state);
		break;
	case VRNG_EVENT_VALIDATE:
		ret = ops->validate(state);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	outcome->result = ret;
	return ret;
}

static void vrng_expect_event_with_ops(struct kunit *test,
				       const struct vrng_core_ops *ops,
				       struct vrng_core_state *spec_state,
				       struct vrng_core_state *candidate_state,
				       const struct vrng_spec_event *event,
				       const u8 *dma_buffer)
{
	struct vrng_spec_outcome spec_outcome, candidate_outcome;
	u8 spec_destination[VRNG_TEST_BUFFER_SIZE];
	u8 candidate_destination[VRNG_TEST_BUFFER_SIZE];
	int spec_ret, candidate_ret;

	memset(spec_destination, 0xa5, sizeof(spec_destination));
	memset(candidate_destination, 0xa5, sizeof(candidate_destination));
	spec_ret = vrng_spec_step(spec_state, event, dma_buffer,
				  spec_destination, &spec_outcome);
	candidate_ret = vrng_run_event(ops, candidate_state, event, dma_buffer,
				       candidate_destination,
				       &candidate_outcome);

	KUNIT_EXPECT_EQ(test, candidate_ret, spec_ret);
	KUNIT_EXPECT_EQ(test, candidate_outcome.result, spec_outcome.result);
	KUNIT_EXPECT_EQ(test, candidate_outcome.copied, spec_outcome.copied);
	KUNIT_EXPECT_EQ(test, candidate_outcome.need_resubmit,
			spec_outcome.need_resubmit);
	KUNIT_EXPECT_EQ(test, candidate_outcome.generation,
			spec_outcome.generation);
	KUNIT_EXPECT_MEMEQ(test, candidate_state, spec_state,
			   sizeof(*candidate_state));
	KUNIT_EXPECT_MEMEQ(test, candidate_destination, spec_destination,
			   sizeof(candidate_destination));
	KUNIT_EXPECT_EQ(test, ops->validate(candidate_state),
			vrng_spec_step(spec_state,
				       &(struct vrng_spec_event){
					       .kind = VRNG_EVENT_VALIDATE,
				       },
				       dma_buffer, spec_destination,
				       &spec_outcome));
}

static void vrng_expect_event(struct kunit *test,
			      struct vrng_core_state *spec_state,
			      struct vrng_core_state *c_state,
			      const struct vrng_spec_event *event,
			      const u8 *dma_buffer)
{
	vrng_expect_event_with_ops(test, &vrng_c_ops, spec_state, c_state,
				   event, dma_buffer);
}

static void vrng_normal_sequence_test(struct kunit *test)
{
	struct vrng_core_state spec_state = {}, c_state = {};
	const u8 dma[VRNG_TEST_BUFFER_SIZE] = {
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	};
	struct vrng_spec_event event = {
		.kind = VRNG_EVENT_INIT,
		.value = 8,
		.epoch = 10,
	};

	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	event = (struct vrng_spec_event){ .kind = VRNG_EVENT_BEGIN_SUBMIT };
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	KUNIT_EXPECT_EQ(test, c_state.generation, 11ULL);

	event = (struct vrng_spec_event){
		.kind = VRNG_EVENT_COMPLETE,
		.value = 6,
		.generation = 11,
	};
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	KUNIT_EXPECT_EQ(test, c_state.phase, (u32)VRNG_READY);

	event = (struct vrng_spec_event){
		.kind = VRNG_EVENT_COPY,
		.value = 2,
	};
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	KUNIT_EXPECT_EQ(test, c_state.data_idx, 2U);
	KUNIT_EXPECT_EQ(test, c_state.data_avail, 4U);

	event.value = 8;
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	KUNIT_EXPECT_EQ(test, c_state.phase, (u32)VRNG_EMPTY);
}

static void vrng_abort_and_stale_test(struct kunit *test)
{
	struct vrng_core_state spec_state = {}, c_state = {};
	u8 dma[VRNG_TEST_BUFFER_SIZE] = {};
	struct vrng_spec_event event = {
		.kind = VRNG_EVENT_INIT,
		.value = 8,
		.epoch = 40,
	};

	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	event = (struct vrng_spec_event){ .kind = VRNG_EVENT_BEGIN_SUBMIT };
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	event = (struct vrng_spec_event){
		.kind = VRNG_EVENT_ABORT_SUBMIT,
		.generation = 40,
	};
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	KUNIT_EXPECT_EQ(test, c_state.phase, (u32)VRNG_DEVICE_OWNED);

	event.generation = 41;
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	KUNIT_EXPECT_EQ(test, c_state.phase, (u32)VRNG_EMPTY);
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
}

static void vrng_completion_error_test(struct kunit *test)
{
	struct vrng_core_state spec_state = {}, c_state = {};
	u8 dma[VRNG_TEST_BUFFER_SIZE] = {};
	struct vrng_spec_event event = {
		.kind = VRNG_EVENT_INIT,
		.value = 8,
		.epoch = 100,
	};

	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	event = (struct vrng_spec_event){ .kind = VRNG_EVENT_BEGIN_SUBMIT };
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	event = (struct vrng_spec_event){
		.kind = VRNG_EVENT_COMPLETE,
		.value = 0,
		.generation = 101,
	};
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	KUNIT_EXPECT_EQ(test, c_state.phase, (u32)VRNG_EMPTY);

	event = (struct vrng_spec_event){ .kind = VRNG_EVENT_BEGIN_SUBMIT };
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	event = (struct vrng_spec_event){
		.kind = VRNG_EVENT_COMPLETE,
		.value = 9,
		.generation = 102,
	};
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	KUNIT_EXPECT_EQ(test, c_state.phase, (u32)VRNG_EMPTY);
}

static void vrng_duplicate_and_copy_state_test(struct kunit *test)
{
	struct vrng_core_state spec_state = {}, c_state = {};
	u8 dma[VRNG_TEST_BUFFER_SIZE] = {};
	struct vrng_spec_event event = {
		.kind = VRNG_EVENT_INIT,
		.value = 8,
		.epoch = 200,
	};

	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	event = (struct vrng_spec_event){ .kind = VRNG_EVENT_COPY, .value = 1 };
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	event = (struct vrng_spec_event){ .kind = VRNG_EVENT_BEGIN_SUBMIT };
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	event = (struct vrng_spec_event){ .kind = VRNG_EVENT_COPY, .value = 1 };
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	event = (struct vrng_spec_event){
		.kind = VRNG_EVENT_COMPLETE,
		.value = 4,
		.generation = 201,
	};
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	event = (struct vrng_spec_event){ .kind = VRNG_EVENT_COPY, .value = 0 };
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
}

static void vrng_remove_sequence_test(struct kunit *test)
{
	struct vrng_core_state spec_state = {}, c_state = {};
	u8 dma[VRNG_TEST_BUFFER_SIZE] = {};
	struct vrng_spec_event event = {
		.kind = VRNG_EVENT_INIT,
		.value = 8,
		.epoch = 300,
	};

	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	event = (struct vrng_spec_event){ .kind = VRNG_EVENT_BEGIN_SUBMIT };
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	event = (struct vrng_spec_event){ .kind = VRNG_EVENT_BEGIN_REMOVE };
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	KUNIT_EXPECT_EQ(test, c_state.lifecycle, (u32)VRNG_QUIESCING);
	KUNIT_EXPECT_EQ(test, c_state.phase, (u32)VRNG_DEVICE_OWNED);

	event = (struct vrng_spec_event){
		.kind = VRNG_EVENT_COMPLETE,
		.value = 4,
		.generation = 301,
	};
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	KUNIT_EXPECT_EQ(test, c_state.phase, (u32)VRNG_EMPTY);
	event = (struct vrng_spec_event){ .kind = VRNG_EVENT_FINISH_REMOVE };
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	KUNIT_EXPECT_EQ(test, c_state.lifecycle, (u32)VRNG_DEAD);
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	event = (struct vrng_spec_event){ .kind = VRNG_EVENT_BEGIN_SUBMIT };
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
}

static void vrng_generation_overflow_test(struct kunit *test)
{
	struct vrng_core_state spec_state = {}, c_state = {};
	u8 dma[VRNG_TEST_BUFFER_SIZE] = {};
	struct vrng_spec_event event = {
		.kind = VRNG_EVENT_INIT,
		.value = 8,
		.epoch = U64_MAX,
	};

	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	event = (struct vrng_spec_event){ .kind = VRNG_EVENT_BEGIN_SUBMIT };
	vrng_expect_event(test, &spec_state, &c_state, &event, dma);
	KUNIT_EXPECT_EQ(test, c_state.phase, (u32)VRNG_EMPTY);
}

static void vrng_invalid_state_and_outputs_test(struct kunit *test)
{
	struct vrng_core_state state;
	u8 dma[8] = {}, destination[8] = {};
	u32 copied = 99, need_resubmit = 99;
	u64 generation = 99;

	KUNIT_ASSERT_EQ(test, vrng_core_c_init(&state, 8, 0), 0);
	state.data_idx = 7;
	state.data_avail = 2;
	KUNIT_EXPECT_EQ(test, vrng_core_c_validate(&state), -EINVAL);
	KUNIT_EXPECT_EQ(test,
			vrng_core_c_copy(&state, dma, destination, 1, &copied,
					 &need_resubmit),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, copied, 0U);
	KUNIT_EXPECT_EQ(test, need_resubmit, 0U);

	KUNIT_EXPECT_EQ(test, vrng_core_c_begin_submit(NULL, &generation),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, generation, 0ULL);
}

static void vrng_copy_contract_for_ops(struct kunit *test,
				       const struct vrng_core_ops *ops)
{
	struct vrng_core_state state, before;
	u8 dma[8] = {}, destination[8] = {};
	u32 copied, need_resubmit;
	u64 generation;
	int ret;

	KUNIT_ASSERT_EQ(test, ops->init(&state, sizeof(dma), 10), 0);
	before = state;
	copied = 99;
	ret = ops->copy(&state, dma, destination, 1, &copied, NULL);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_EQ(test, copied, 0U);
	KUNIT_EXPECT_MEMEQ(test, &state, &before, sizeof(state));

	need_resubmit = 99;
	ret = ops->copy(&state, dma, destination, 1, NULL, &need_resubmit);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_EQ(test, need_resubmit, 0U);
	KUNIT_EXPECT_MEMEQ(test, &state, &before, sizeof(state));

	copied = 99;
	need_resubmit = 99;
	ret = ops->copy(&state, NULL, destination, 1, &copied, &need_resubmit);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_EQ(test, copied, 0U);
	KUNIT_EXPECT_EQ(test, need_resubmit, 0U);
	KUNIT_EXPECT_MEMEQ(test, &state, &before, sizeof(state));

	ret = ops->copy(&state, dma, destination, 1, &copied, &need_resubmit);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_MEMEQ(test, &state, &before, sizeof(state));

	KUNIT_ASSERT_EQ(test, ops->begin_submit(&state, &generation), 0);
	before = state;
	ret = ops->copy(&state, NULL, destination, 1, &copied, &need_resubmit);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = ops->copy(&state, dma, destination, 1, &copied, &need_resubmit);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	KUNIT_EXPECT_MEMEQ(test, &state, &before, sizeof(state));

	KUNIT_ASSERT_EQ(test, ops->abort_submit(&state, generation), 0);
	KUNIT_ASSERT_EQ(test, ops->begin_remove(&state), 0);
	before = state;
	ret = ops->copy(&state, NULL, destination, 1, &copied, &need_resubmit);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = ops->copy(&state, dma, destination, 1, &copied, &need_resubmit);
	KUNIT_EXPECT_EQ(test, ret, -ENODEV);
	KUNIT_EXPECT_MEMEQ(test, &state, &before, sizeof(state));
}

static void vrng_c_copy_contract_test(struct kunit *test)
{
	vrng_copy_contract_for_ops(test, &vrng_c_ops);
}

static void vrng_partial_copy_accounting_test(struct kunit *test)
{
	u8 source[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	u8 destination[8] = {};
	u32 index = 0, remaining = sizeof(source);
	int copied;

	copied = virtrng_copy_available(source, sizeof(source), &index,
					&remaining, destination, 3);
	KUNIT_ASSERT_EQ(test, copied, 3);
	KUNIT_EXPECT_EQ(test, index, 3U);
	KUNIT_EXPECT_EQ(test, remaining, 5U);

	copied = virtrng_copy_available(source, sizeof(source), &index,
					&remaining, destination + 3, 3);
	KUNIT_ASSERT_EQ(test, copied, 3);
	KUNIT_EXPECT_EQ(test, index, 6U);
	KUNIT_EXPECT_EQ(test, remaining, 2U);

	copied = virtrng_copy_available(source, sizeof(source), &index,
					&remaining, destination + 6, 3);
	KUNIT_ASSERT_EQ(test, copied, 2);
	KUNIT_EXPECT_EQ(test, index, 8U);
	KUNIT_EXPECT_EQ(test, remaining, 0U);
	KUNIT_EXPECT_MEMEQ(test, destination, source, sizeof(source));

	index = 5;
	remaining = 4;
	copied = virtrng_copy_available(source, sizeof(source), &index,
					&remaining, destination, 1);
	KUNIT_EXPECT_EQ(test, copied, -EOVERFLOW);
	KUNIT_EXPECT_EQ(test, index, 5U);
	KUNIT_EXPECT_EQ(test, remaining, 4U);
}

static void vrng_persistent_read_error_test(struct kunit *test)
{
	int transient = -ENOSPC;

	KUNIT_EXPECT_EQ(test, virtrng_read_error(-EIO, &transient), -EIO);
	KUNIT_EXPECT_EQ(test, virtrng_read_error(-EIO, &transient), -EIO);
	KUNIT_EXPECT_EQ(test, transient, -ENOSPC);
	KUNIT_EXPECT_EQ(test, virtrng_read_error(0, &transient), -ENOSPC);
	KUNIT_EXPECT_EQ(test, virtrng_read_error(0, &transient), 0);
}

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
static void vrng_shadow_copy_output_validation_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, vrng_shadow_copy_output_valid(0, 4, 8, 4, 0));
	KUNIT_EXPECT_TRUE(test, vrng_shadow_copy_output_valid(0, 8, 8, 8, 1));
	KUNIT_EXPECT_TRUE(test,
			  vrng_shadow_copy_output_valid(-EAGAIN, 4, 0, 0, 0));
	KUNIT_EXPECT_FALSE(test, vrng_shadow_copy_output_valid(0, 4, 8, 5, 0));
	KUNIT_EXPECT_FALSE(test, vrng_shadow_copy_output_valid(0, 8, 4, 5, 0));
	KUNIT_EXPECT_FALSE(test, vrng_shadow_copy_output_valid(0, 4, 8, 4, 2));
	KUNIT_EXPECT_FALSE(test,
			   vrng_shadow_copy_output_valid(-EAGAIN, 4, 8, 1, 0));
	KUNIT_EXPECT_FALSE(test, vrng_shadow_copy_output_valid(1, 4, 8, 0, 0));
	KUNIT_EXPECT_FALSE(test, vrng_shadow_copy_output_valid(0, 4, 8, 0, 0));
	KUNIT_EXPECT_FALSE(test, vrng_shadow_copy_output_valid(0, 4, 8, 2, 0));
}

static void vrng_shadow_control_validation_test(struct kunit *test)
{
	struct vrng_core_state state = {
		.abi_version = VRNG_CORE_ABI_VERSION,
		.lifecycle = VRNG_ACTIVE,
		.phase = VRNG_EMPTY,
		.capacity = 8,
	};
	struct vrng_core_state spec_state = state;
	struct vrng_shadow_mismatch last;
	struct vrng_shadow shadow;
	u64 events, generation = 99, mismatches;

	KUNIT_EXPECT_FALSE(test, vrng_shadow_control_valid(1, 0, 0, 0, &state,
							   &spec_state));
	KUNIT_EXPECT_TRUE(test, vrng_shadow_control_valid(0, 0, 4, 4, &state,
							  &spec_state));
	state.capacity = 7;
	KUNIT_EXPECT_FALSE(test, vrng_shadow_control_valid(0, 0, 4, 4, &state,
							   &spec_state));

	KUNIT_ASSERT_EQ(test, vrng_shadow_init(&shadow, 8), 0);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_CONTROL_RUST)
	shadow.rust_state.capacity = 7;
#elif IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_CONTROL_MC)
	shadow.mc_state.capacity = 7;
#else
	shadow.c_state.capacity = 7;
#endif
	KUNIT_EXPECT_EQ(test, vrng_shadow_begin_submit(&shadow, &generation),
			-EPROTO);
	KUNIT_EXPECT_EQ(test, generation, 0ULL);
	vrng_shadow_snapshot(&shadow, &events, &mismatches, &last);
	KUNIT_EXPECT_EQ(test, events, 2ULL);
	KUNIT_EXPECT_EQ(test, mismatches, 1ULL);
	KUNIT_EXPECT_EQ(test, last.spec_result, 0);
}
#endif

struct vrng_bfs_node {
	struct vrng_core_state state;
	u32 depth;
};

static bool vrng_bfs_contains(const struct vrng_bfs_node *nodes, u32 count,
			      const struct vrng_core_state *state)
{
	u32 i;

	for (i = 0; i < count; i++) {
		if (!memcmp(&nodes[i].state, state, sizeof(*state)))
			return true;
	}
	return false;
}

static void vrng_bounded_state_space(struct kunit *test,
				     const struct vrng_core_ops *ops)
{
	struct vrng_bfs_node *nodes;
	struct vrng_core_state spec_state = {}, c_state = {};
	struct vrng_spec_event events[12];
	u8 dma[VRNG_TEST_BUFFER_SIZE];
	u32 count = 0, cursor, event_count, i;

	nodes = kunit_kcalloc(test, VRNG_BFS_MAX_STATES, sizeof(*nodes),
			      GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, nodes);
	for (i = 0; i < ARRAY_SIZE(dma); i++)
		dma[i] = i;

	vrng_expect_event_with_ops(test, ops, &spec_state, &c_state,
				   &(struct vrng_spec_event){
					   .kind = VRNG_EVENT_INIT,
					   .value = 3,
					   .epoch = 0,
				   },
				   dma);
	nodes[count++] = (struct vrng_bfs_node){
		.state = c_state,
		.depth = 0,
	};

	for (cursor = 0; cursor < count; cursor++) {
		const struct vrng_core_state *base = &nodes[cursor].state;

		if (nodes[cursor].depth == VRNG_BFS_DEPTH)
			continue;

		event_count = 0;
		events[event_count++] = (struct vrng_spec_event){
			.kind = VRNG_EVENT_BEGIN_SUBMIT,
		};
		events[event_count++] = (struct vrng_spec_event){
			.kind = VRNG_EVENT_ABORT_SUBMIT,
			.generation = base->generation,
		};
		events[event_count++] = (struct vrng_spec_event){
			.kind = VRNG_EVENT_ABORT_SUBMIT,
			.generation = base->generation + 1,
		};
		events[event_count++] = (struct vrng_spec_event){
			.kind = VRNG_EVENT_COMPLETE,
			.value = 0,
			.generation = base->generation,
		};
		events[event_count++] = (struct vrng_spec_event){
			.kind = VRNG_EVENT_COMPLETE,
			.value = 1,
			.generation = base->generation,
		};
		events[event_count++] = (struct vrng_spec_event){
			.kind = VRNG_EVENT_COMPLETE,
			.value = base->capacity,
			.generation = base->generation,
		};
		events[event_count++] = (struct vrng_spec_event){
			.kind = VRNG_EVENT_COMPLETE,
			.value = base->capacity + 1,
			.generation = base->generation,
		};
		events[event_count++] = (struct vrng_spec_event){
			.kind = VRNG_EVENT_COMPLETE,
			.value = 1,
			.generation = base->generation + 1,
		};
		events[event_count++] = (struct vrng_spec_event){
			.kind = VRNG_EVENT_COPY,
			.value = 0,
		};
		events[event_count++] = (struct vrng_spec_event){
			.kind = VRNG_EVENT_COPY,
			.value = 1,
		};
		events[event_count++] = (struct vrng_spec_event){
			.kind = VRNG_EVENT_BEGIN_REMOVE,
		};
		events[event_count++] = (struct vrng_spec_event){
			.kind = VRNG_EVENT_FINISH_REMOVE,
		};

		for (i = 0; i < event_count; i++) {
			spec_state = *base;
			c_state = *base;
			vrng_expect_event_with_ops(test, ops, &spec_state,
						   &c_state, &events[i], dma);
			if (vrng_bfs_contains(nodes, count, &c_state))
				continue;
			KUNIT_ASSERT_LT(test, count, VRNG_BFS_MAX_STATES);
			nodes[count++] = (struct vrng_bfs_node){
				.state = c_state,
				.depth = nodes[cursor].depth + 1,
			};
		}
	}

	KUNIT_EXPECT_GT(test, count, 20U);
}

static void vrng_bounded_state_space_test(struct kunit *test)
{
	vrng_bounded_state_space(test, &vrng_c_ops);
}

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST) || \
	IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
static void vrng_candidate_directed(struct kunit *test,
				    const struct vrng_core_ops *ops)
{
	struct vrng_core_state spec_state = {}, candidate_state = {};
	u8 dma[VRNG_TEST_BUFFER_SIZE] = {};
	struct vrng_spec_event events[] = {
		{ .kind = VRNG_EVENT_INIT, .value = 8, .epoch = 500 },
		{ .kind = VRNG_EVENT_BEGIN_SUBMIT },
		{ .kind = VRNG_EVENT_COMPLETE, .value = 6, .generation = 501 },
		{ .kind = VRNG_EVENT_COPY, .value = 2 },
		{ .kind = VRNG_EVENT_COPY, .value = 8 },
		{ .kind = VRNG_EVENT_BEGIN_SUBMIT },
		{ .kind = VRNG_EVENT_ABORT_SUBMIT, .generation = 502 },
		{ .kind = VRNG_EVENT_BEGIN_SUBMIT },
		{ .kind = VRNG_EVENT_BEGIN_REMOVE },
		{ .kind = VRNG_EVENT_COMPLETE, .value = 4, .generation = 503 },
		{ .kind = VRNG_EVENT_FINISH_REMOVE },
	};
	u32 i;

	for (i = 0; i < ARRAY_SIZE(events); i++)
		vrng_expect_event_with_ops(test, ops, &spec_state,
					   &candidate_state, &events[i], dma);

	vrng_expect_event_with_ops(test, ops, &spec_state, &candidate_state,
				   &(struct vrng_spec_event){
					   .kind = VRNG_EVENT_INIT,
					   .value = 8,
					   .epoch = U64_MAX,
				   },
				   dma);
	vrng_expect_event_with_ops(test, ops, &spec_state, &candidate_state,
				   &(struct vrng_spec_event){
					   .kind = VRNG_EVENT_BEGIN_SUBMIT,
				   },
				   dma);
}
#endif

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
static void vrng_rust_directed_test(struct kunit *test)
{
	vrng_candidate_directed(test, &vrng_rust_ops);
}

static void vrng_rust_bounded_state_space_test(struct kunit *test)
{
	vrng_bounded_state_space(test, &vrng_rust_ops);
}

static void vrng_rust_copy_contract_test(struct kunit *test)
{
	vrng_copy_contract_for_ops(test, &vrng_rust_ops);
}
#endif

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
static void vrng_mc_directed_test(struct kunit *test)
{
	vrng_candidate_directed(test, &vrng_mc_ops);
}

static void vrng_mc_bounded_state_space_test(struct kunit *test)
{
	vrng_bounded_state_space(test, &vrng_mc_ops);
}

static void vrng_mc_copy_contract_test(struct kunit *test)
{
	vrng_copy_contract_for_ops(test, &vrng_mc_ops);
}
#endif

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
static void vrng_shadow_normal_sequence_test(struct kunit *test)
{
	struct vrng_shadow_mismatch last;
	struct vrng_shadow shadow;
	u8 dma[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	u8 destination[8] = {};
	u32 copied, need_resubmit;
	u64 generation;
	u64 events, mismatches;

	KUNIT_ASSERT_EQ(test, vrng_shadow_init(&shadow, sizeof(dma)), 0);
	KUNIT_ASSERT_EQ(test, vrng_shadow_begin_submit(&shadow, &generation),
			0);
	KUNIT_ASSERT_EQ(test,
			vrng_shadow_complete(&shadow, generation, sizeof(dma),
					     &need_resubmit),
			0);
	KUNIT_ASSERT_EQ(test,
			vrng_shadow_copy(&shadow, dma, destination, 3, &copied,
					 &need_resubmit),
			0);
	KUNIT_EXPECT_EQ(test, copied, 3U);
	KUNIT_ASSERT_EQ(test,
			vrng_shadow_copy(&shadow, dma, destination + 3, 5,
					 &copied, &need_resubmit),
			0);
	KUNIT_EXPECT_EQ(test, need_resubmit, 1U);
	KUNIT_ASSERT_EQ(test, vrng_shadow_begin_submit(&shadow, &generation),
			0);
	vrng_shadow_begin_remove(&shadow);
	vrng_shadow_finish_remove(&shadow);
	vrng_shadow_snapshot(&shadow, &events, &mismatches, &last);

	KUNIT_EXPECT_EQ(test, events, 8ULL);
	KUNIT_EXPECT_EQ(test, mismatches, 0ULL);
	KUNIT_EXPECT_FALSE(test, shadow.active);
	KUNIT_EXPECT_EQ(test, shadow.c_state.lifecycle, (u32)VRNG_DEAD);
}

static void vrng_shadow_abort_sequence_test(struct kunit *test)
{
	struct vrng_shadow_mismatch last;
	struct vrng_shadow shadow;
	u64 generation;
	u64 events, mismatches;

	KUNIT_ASSERT_EQ(test, vrng_shadow_init(&shadow, 8), 0);
	KUNIT_ASSERT_EQ(test, vrng_shadow_begin_submit(&shadow, &generation),
			0);
	KUNIT_ASSERT_EQ(test, vrng_shadow_abort_submit(&shadow, generation), 0);
	vrng_shadow_begin_remove(&shadow);
	vrng_shadow_finish_remove(&shadow);
	vrng_shadow_snapshot(&shadow, &events, &mismatches, &last);

	KUNIT_EXPECT_EQ(test, events, 5ULL);
	KUNIT_EXPECT_EQ(test, mismatches, 0ULL);
	KUNIT_EXPECT_EQ(test, shadow.c_state.lifecycle, (u32)VRNG_DEAD);
}

static void vrng_shadow_cookie_generation_test(struct kunit *test)
{
	struct vrng_shadow_mismatch last;
	struct vrng_shadow shadow;
	u32 need_resubmit = 99;
	u64 events, generation, mismatches;

	KUNIT_ASSERT_EQ(test, vrng_shadow_init(&shadow, 8), 0);
	KUNIT_ASSERT_EQ(test, vrng_shadow_begin_submit(&shadow, &generation),
			0);
	KUNIT_EXPECT_EQ(test,
			vrng_shadow_complete(&shadow, generation - 1, 4,
					     &need_resubmit),
			-ESTALE);
	KUNIT_EXPECT_EQ(test, need_resubmit, 0U);
	KUNIT_EXPECT_EQ(test, shadow.c_state.phase, (u32)VRNG_DEVICE_OWNED);
	KUNIT_ASSERT_EQ(test, vrng_shadow_recover_consumed(&shadow), 0);
	KUNIT_EXPECT_EQ(test, shadow.c_state.phase, (u32)VRNG_EMPTY);
	KUNIT_ASSERT_EQ(test, vrng_shadow_begin_submit(&shadow, &generation),
			0);
	KUNIT_EXPECT_EQ(test,
			vrng_shadow_complete(&shadow, generation, 4,
					     &need_resubmit),
			0);
	vrng_shadow_snapshot(&shadow, &events, &mismatches, &last);
	KUNIT_EXPECT_EQ(test, events, 6ULL);
	KUNIT_EXPECT_EQ(test, mismatches, 0ULL);
}

static void vrng_shadow_selected_control_test(struct kunit *test)
{
	struct vrng_shadow_mismatch last;
	struct vrng_shadow shadow;
	u64 events, generation, mismatches;

	KUNIT_ASSERT_EQ(test, vrng_shadow_init(&shadow, 8), 0);
	KUNIT_EXPECT_EQ(test, vrng_shadow_current_epoch(&shadow),
			shadow.spec_state.epoch);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_CONTROL_RUST)
	KUNIT_EXPECT_STREQ(test, vrng_shadow_control_name(), "Rust");
	shadow.rust_state.capacity = 0;
#elif IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_CONTROL_MC)
	KUNIT_EXPECT_STREQ(test, vrng_shadow_control_name(), "MC");
	shadow.mc_state.capacity = 0;
#else
	KUNIT_EXPECT_STREQ(test, vrng_shadow_control_name(), "C");
	shadow.c_state.capacity = 0;
#endif
	KUNIT_EXPECT_EQ(test,
			vrng_shadow_begin_submit(&shadow, &generation), -EPROTO);
	vrng_shadow_snapshot(&shadow, &events, &mismatches, &last);
	KUNIT_EXPECT_EQ(test, events, 2ULL);
	KUNIT_EXPECT_EQ(test, mismatches, 1ULL);
	KUNIT_EXPECT_EQ(test, last.event, (u32)VRNG_SHADOW_BEGIN_SUBMIT);
}

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST) || \
	IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
static void vrng_shadow_mismatch_record_test(struct kunit *test)
{
	struct vrng_shadow_mismatch last;
	struct vrng_shadow shadow;
	u64 generation;
	u64 events, mismatches;

	KUNIT_ASSERT_EQ(test, vrng_shadow_init(&shadow, 8), 0);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	shadow.rust_state.capacity = 0;
#else
	shadow.mc_state.capacity = 0;
#endif
	vrng_shadow_begin_submit(&shadow, &generation);
	vrng_shadow_snapshot(&shadow, &events, &mismatches, &last);

	KUNIT_EXPECT_EQ(test, events, 2ULL);
	KUNIT_EXPECT_EQ(test, mismatches, 1ULL);
	KUNIT_EXPECT_EQ(test, last.event, (u32)VRNG_SHADOW_BEGIN_SUBMIT);
	KUNIT_EXPECT_EQ(test, last.sequence, 2ULL);
}
#endif
#endif

static struct kunit_case vrng_core_test_cases[] = {
	KUNIT_CASE(vrng_normal_sequence_test),
	KUNIT_CASE(vrng_abort_and_stale_test),
	KUNIT_CASE(vrng_completion_error_test),
	KUNIT_CASE(vrng_duplicate_and_copy_state_test),
	KUNIT_CASE(vrng_remove_sequence_test),
	KUNIT_CASE(vrng_generation_overflow_test),
	KUNIT_CASE(vrng_invalid_state_and_outputs_test),
	KUNIT_CASE(vrng_c_copy_contract_test),
	KUNIT_CASE(vrng_partial_copy_accounting_test),
	KUNIT_CASE(vrng_persistent_read_error_test),
	KUNIT_CASE(vrng_bounded_state_space_test),
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST)
	KUNIT_CASE(vrng_rust_directed_test),
	KUNIT_CASE(vrng_rust_bounded_state_space_test),
	KUNIT_CASE(vrng_rust_copy_contract_test),
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	KUNIT_CASE(vrng_mc_directed_test),
	KUNIT_CASE(vrng_mc_bounded_state_space_test),
	KUNIT_CASE(vrng_mc_copy_contract_test),
#endif
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	KUNIT_CASE(vrng_shadow_copy_output_validation_test),
	KUNIT_CASE(vrng_shadow_control_validation_test),
	KUNIT_CASE(vrng_shadow_normal_sequence_test),
	KUNIT_CASE(vrng_shadow_abort_sequence_test),
	KUNIT_CASE(vrng_shadow_cookie_generation_test),
	KUNIT_CASE(vrng_shadow_selected_control_test),
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_RUST) || \
	IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_MC)
	KUNIT_CASE(vrng_shadow_mismatch_record_test),
#endif
#endif
	{}
};

static struct kunit_suite vrng_core_test_suite = {
	.name = "virtio-rng-lang-core",
	.test_cases = vrng_core_test_cases,
};

kunit_test_suite(vrng_core_test_suite);

MODULE_DESCRIPTION("KUnit tests for the virtio-rng language core experiment");
MODULE_LICENSE("GPL");
