// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Randomness driver for virtio
 *  Copyright (C) 2007, 2008 Rusty Russell IBM Corporation
 */

#include <asm/barrier.h>
#include <linux/err.h>
#include <linux/hw_random.h>
#include <linux/nospec.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/virtio.h>
#include <linux/virtio_rng.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
#include "virtio_rng_lang/vrng_shadow.h"
#include "virtio_rng_lang/vrng_driver_shadow.h"
#endif
#include "virtio_rng_internal.h"

static DEFINE_IDA(rng_index_ida);

static unsigned int vrng_copy_chunk_limit;
module_param_named(lang_copy_chunk_limit, vrng_copy_chunk_limit, uint, 0600);
MODULE_PARM_DESC(lang_copy_chunk_limit,
		 "limit bytes copied from one virtio completion per driver read");

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
static int vrng_lang_fail_add_once;
static int vrng_lang_completion_override = -1;
static int vrng_lang_stale_once;
static int vrng_lang_hold_completion;
static int vrng_lang_completion_held;
static int vrng_lang_hold_publication;
static int vrng_lang_publication_held;
static int vrng_lang_fail_register_once;
static int vrng_lang_fail_teardown_once;
static int vrng_lang_remove_begun;
static int vrng_lang_last_remove_data_avail = -1;
static int vrng_lang_last_remove_lifecycle = -1;
static int vrng_lang_last_remove_phase = -1;
struct vrng_teardown_record {
	u64 sequence;
	u64 device_cookie;
	s32 teardown_error;
	s32 stage;
	s32 external_avail;
	u64 events;
	u64 mismatches;
};

static DEFINE_MUTEX(vrng_teardown_record_lock);
static struct vrng_teardown_record vrng_last_teardown_record = {
	.stage = -1,
	.external_avail = -1,
};

static u64 vrng_next_device_cookie;

static int vrng_teardown_record_get(char *buffer,
				    const struct kernel_param *kp)
{
	struct vrng_teardown_record record;

	(void)kp;
	mutex_lock(&vrng_teardown_record_lock);
	record = vrng_last_teardown_record;
	mutex_unlock(&vrng_teardown_record_lock);

	return scnprintf(buffer, PAGE_SIZE,
			 "sequence=%llu device=%llu error=%d stage=%d avail=%d events=%llu mismatches=%llu\n",
			 record.sequence, record.device_cookie,
			 record.teardown_error, record.stage,
			 record.external_avail, record.events,
			 record.mismatches);
}

static const struct kernel_param_ops vrng_teardown_record_ops = {
	.get = vrng_teardown_record_get,
};
module_param_named(lang_fail_add_once, vrng_lang_fail_add_once, int, 0600);
module_param_named(lang_completion_override, vrng_lang_completion_override,
		   int, 0600);
module_param_named(lang_stale_once, vrng_lang_stale_once, int, 0600);
module_param_named(lang_hold_completion, vrng_lang_hold_completion, int, 0600);
module_param_named(lang_completion_held, vrng_lang_completion_held, int, 0400);
module_param_named(lang_hold_publication, vrng_lang_hold_publication, int, 0600);
module_param_named(lang_publication_held, vrng_lang_publication_held, int, 0400);
module_param_named(lang_fail_register_once, vrng_lang_fail_register_once, int,
		   0600);
module_param_named(lang_fail_teardown_once, vrng_lang_fail_teardown_once, int,
		   0600);
module_param_named(lang_remove_begun, vrng_lang_remove_begun, int, 0400);
module_param_named(lang_last_remove_data_avail,
		   vrng_lang_last_remove_data_avail, int, 0400);
module_param_named(lang_last_remove_lifecycle,
		   vrng_lang_last_remove_lifecycle, int, 0400);
module_param_named(lang_last_remove_phase, vrng_lang_last_remove_phase, int,
		   0400);
module_param_cb(lang_last_driver_record, &vrng_teardown_record_ops, NULL, 0400);
MODULE_PARM_DESC(lang_fail_add_once,
		 "fail the next experimental virtqueue submission");
MODULE_PARM_DESC(lang_completion_override,
		 "replace the next experimental completion length (-1 disables)");
MODULE_PARM_DESC(lang_stale_once,
		 "use a stale generation for the next experimental completion");
MODULE_PARM_DESC(lang_hold_completion,
		 "consume and hold the next experimental completion for removal tests");
MODULE_PARM_DESC(lang_completion_held,
		 "indicate that an experimental completion is currently held");
MODULE_PARM_DESC(lang_hold_publication,
		 "pause a completion after logical completion and before publication");
MODULE_PARM_DESC(lang_publication_held,
		 "indicate that an experimental completion publication is paused");
MODULE_PARM_DESC(lang_fail_register_once,
		 "fail the next experimental hwrng registration");
MODULE_PARM_DESC(lang_fail_teardown_once,
		 "inject a logical teardown error after physical cleanup");
MODULE_PARM_DESC(lang_remove_begun,
		 "indicate that experimental logical removal has begun");
MODULE_PARM_DESC(lang_last_remove_data_avail,
		 "external availability recorded after the last callback drain");
MODULE_PARM_DESC(lang_last_remove_lifecycle,
		 "logical lifecycle recorded after the last removal");
MODULE_PARM_DESC(lang_last_remove_phase,
		 "logical phase recorded after the last removal");
MODULE_PARM_DESC(lang_last_driver_record,
		 "coherent sequenced driver lifecycle record after removal");
#endif

struct virtrng_info;

struct vrng_request_cookie {
	struct virtrng_info *vi;
	u64 epoch;
	u64 generation;
	u64 request_id;
};

struct virtrng_info {
	struct hwrng hwrng;
	struct virtqueue *vq;
	char name[25];
	int index;
	bool hwrng_register_done;
	bool hwrng_removed;
	bool cleanup_pending;
	int fatal_errno;
	/* Serializes process-context copy, resubmit, retry, and removal. */
	struct mutex process_lock;
	struct delayed_work refill_work;
	int data_error;
	struct vrng_request_cookie cookie;
	u64 next_request_id;
	/* data transfer */
	struct completion have_data;
	unsigned int data_avail;
	unsigned int data_idx;
	/* minimal size returned by rng_buffer_size() */
	__dma_from_device_group_begin();
#if SMP_CACHE_BYTES < 32
	u8 data[32];
#else
	u8 data[SMP_CACHE_BYTES];
#endif
	__dma_from_device_group_end();
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	struct vrng_shadow shadow;
	struct vrng_driver_shadow driver_shadow;
	struct work_struct publication_work;
	unsigned int publication_len;
	bool shadow_initialized;
	bool shadow_active;
	bool driver_shadow_initialized;
	u64 evidence_cookie;
#endif
};

static int request_entropy_locked(struct virtrng_info *vi);
static int remove_common(struct virtio_device *vdev);

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
static void publish_fatal_error(struct virtrng_info *vi, int error);

static void publish_held_data(struct work_struct *work)
{
	struct virtrng_info *vi =
		container_of(work, struct virtrng_info, publication_work);
	int err;

	WRITE_ONCE(vrng_lang_publication_held, 1);
	while (READ_ONCE(vrng_lang_hold_publication))
		usleep_range(1000, 2000);
	err = vrng_driver_shadow_publish(&vi->driver_shadow);
	if (err) {
		publish_fatal_error(vi, err);
		WRITE_ONCE(vrng_lang_publication_held, 0);
		return;
	}
	/* Publish the completed DMA bytes before waking readers. */
	smp_store_release(&vi->data_avail, READ_ONCE(vi->publication_len));
	complete(&vi->have_data);
	WRITE_ONCE(vrng_lang_publication_held, 0);
}
#endif

static void publish_data_error(struct virtrng_info *vi, int error)
{
	if (WARN_ON_ONCE(error >= 0))
		error = -EPROTO;
	WRITE_ONCE(vi->data_error, error);
	/* Publish the error before waking readers that acquire data_avail. */
	smp_store_release(&vi->data_avail, 0);
	complete(&vi->have_data);
}

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
static void publish_fatal_error(struct virtrng_info *vi, int error)
{
	if (error >= 0)
		error = -EIO;
	cmpxchg(&vi->fatal_errno, 0, error);
	/* Fatal errors are terminal and must remain visible to every reader. */
	smp_store_release(&vi->data_avail, 0);
	complete_all(&vi->have_data);
}
#endif

static void publish_request_error(struct virtrng_info *vi, int error)
{
	if (!READ_ONCE(vi->fatal_errno))
		publish_data_error(vi, error);
}

static void refill_entropy(struct work_struct *work)
{
	struct virtrng_info *vi =
		container_of(to_delayed_work(work), struct virtrng_info,
			     refill_work);
	int err;

	mutex_lock(&vi->process_lock);
	if (vi->hwrng_removed)
		goto unlock;
	err = request_entropy_locked(vi);
	if (err)
		publish_request_error(vi, err);
unlock:
	mutex_unlock(&vi->process_lock);
}

static void random_recv_done(struct virtqueue *vq)
{
	struct virtrng_info *vi = vq->vdev->priv;
	struct vrng_request_cookie *cookie;
	unsigned int len;
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	u64 generation;
	int err, recovery_err;
#endif

	/* We can get spurious callbacks, e.g. shared IRQs + virtio_pci. */
	cookie = virtqueue_get_buf(vi->vq, &len);
	if (!cookie)
		return;

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	if (WARN_ON_ONCE(cookie != &vi->cookie || cookie->vi != vi)) {
		publish_fatal_error(vi, -EPROTO);
		return;
	}
	if (READ_ONCE(vrng_lang_hold_completion)) {
		WRITE_ONCE(vrng_lang_completion_held, 1);
		return;
	}
	if (READ_ONCE(vrng_lang_completion_override) >= 0)
		len = xchg(&vrng_lang_completion_override, -1);
	generation = cookie->generation;
	if (xchg(&vrng_lang_stale_once, 0))
		generation--;
	err = vrng_shadow_complete(&vi->shadow, generation, len, NULL);
	if (err) {
		if (err == -EPROTO) {
			publish_fatal_error(vi, err);
			return;
		}
		if (err == -ESTALE) {
			recovery_err = vrng_shadow_recover_consumed(&vi->shadow);
			if (recovery_err) {
				publish_fatal_error(vi, recovery_err);
				return;
			}
		}
		publish_data_error(vi, err);
		return;
	}
	err = vrng_driver_shadow_complete(&vi->driver_shadow, len);
	if (err) {
		publish_fatal_error(vi, err);
		return;
	}
	if (READ_ONCE(vrng_lang_hold_publication)) {
		WRITE_ONCE(vi->publication_len, len);
		schedule_work(&vi->publication_work);
		return;
	}
	err = vrng_driver_shadow_publish(&vi->driver_shadow);
	if (err) {
		publish_fatal_error(vi, err);
		return;
	}
#else
	if (!len || len > sizeof(vi->data)) {
		publish_data_error(vi, len ? -EOVERFLOW : -ENODATA);
		return;
	}
#endif
	/* Publish the completed DMA bytes before waking readers. */
	smp_store_release(&vi->data_avail, len);
	complete(&vi->have_data);
}

static int request_entropy_locked(struct virtrng_info *vi)
{
	struct scatterlist sg;
	void *token = vi->data;
	int err;
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	u64 generation = 0;
#endif

	lockdep_assert_held(&vi->process_lock);
	if (vi->hwrng_removed)
		return -ENODEV;
	if (READ_ONCE(vi->fatal_errno))
		return READ_ONCE(vi->fatal_errno);

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	err = vrng_shadow_begin_submit(&vi->shadow, &generation);
	if (err) {
		if (err == -EPROTO)
			publish_fatal_error(vi, err);
		return err;
	}
	vi->cookie.vi = vi;
	vi->cookie.epoch = vrng_shadow_current_epoch(&vi->shadow);
	vi->cookie.generation = generation;
	vi->cookie.request_id = ++vi->next_request_id;
	token = &vi->cookie;
#endif

	reinit_completion(&vi->have_data);
	vi->data_idx = 0;
	sg_init_one(&sg, vi->data, sizeof(vi->data));
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	if (xchg(&vrng_lang_fail_add_once, 0))
		err = -ENOSPC;
	else
#endif
		err = virtqueue_add_inbuf(vi->vq, &sg, 1, token, GFP_KERNEL);
	if (err) {
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
		int abort_err;

		abort_err = vrng_shadow_abort_submit(&vi->shadow, generation);
		if (abort_err) {
			publish_fatal_error(vi, abort_err);
			return abort_err;
		}
#endif
		return err;
	}
	virtqueue_kick(vi->vq);
	return 0;
}

static int copy_data_locked(struct virtrng_info *vi, void *buf,
			    unsigned int size)
{
	int err;
	unsigned int chunk_limit = READ_ONCE(vrng_copy_chunk_limit);

	if (chunk_limit)
		size = min(size, chunk_limit);

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	u32 copied = 0, need_resubmit = 0;

	err = vrng_shadow_copy(&vi->shadow, vi->data, buf, size, &copied,
			       &need_resubmit);
	if (err) {
		if (err == -EPROTO)
			publish_fatal_error(vi, err);
		return err;
	}
	vi->data_idx += copied;
	vi->data_avail -= copied;
	if (need_resubmit) {
		err = request_entropy_locked(vi);
		if (err)
			publish_request_error(vi, err);
	}
	return copied;
#else
	int copied;

	lockdep_assert_held(&vi->process_lock);

	/*
	 * vi->data_avail was set from the device-reported used.len and
	 * vi->data_idx was advanced by previous copy_data() calls.  A
	 * malicious or buggy virtio-rng backend can drive their sum past
	 * sizeof(vi->data). Validate the absolute-index/remaining-length pair
	 * and harden the index with array_index_nospec() so the copy cannot be
	 * steered into adjacent slab memory, including under speculation.
	 */
	copied = virtrng_copy_available(vi->data, sizeof(vi->data),
					&vi->data_idx, &vi->data_avail, buf,
					size);
	if (copied < 0) {
		vi->data_idx = 0;
		vi->data_avail = 0;
		err = request_entropy_locked(vi);
		if (err)
			publish_request_error(vi, err);
		return copied;
	}
	if (vi->data_avail == 0) {
		err = request_entropy_locked(vi);
		if (err)
			publish_request_error(vi, err);
	}
	return copied;
#endif
}

static int virtio_read(struct hwrng *rng, void *buf, size_t size, bool wait)
{
	int ret;
	struct virtrng_info *vi = (struct virtrng_info *)rng->priv;
	int chunk, fatal_error, pending_error;
	size_t read;

	read = 0;
	while (size) {
		if (mutex_lock_interruptible(&vi->process_lock))
			return read ?: -ERESTARTSYS;
		if (vi->hwrng_removed) {
			mutex_unlock(&vi->process_lock);
			return read ?: -ENODEV;
		}
		if (READ_ONCE(vi->cleanup_pending)) {
			WRITE_ONCE(vi->cleanup_pending, false);
			mutex_unlock(&vi->process_lock);
			return read;
		}
		fatal_error = READ_ONCE(vi->fatal_errno);
		pending_error = virtrng_read_error(fatal_error,
						   &vi->data_error);
		if (pending_error) {
			if (!fatal_error)
				mod_delayed_work(system_dfl_wq, &vi->refill_work, 0);
			mutex_unlock(&vi->process_lock);
			return read ?: pending_error;
		}
		chunk = 0;
		/* Pairs with callback/error publication before reading the buffer. */
		if (smp_load_acquire(&vi->data_avail))
			chunk = copy_data_locked(vi, buf + read, size);
		mutex_unlock(&vi->process_lock);
		if (chunk < 0)
			return read ?: chunk;
		size -= chunk;
		read += chunk;
		if (!size || !wait)
			return read;
		/* Consume remaining bytes before waiting for another completion. */
		if (chunk)
			continue;
		ret = wait_for_completion_killable(&vi->have_data);
		if (ret < 0)
			return read ?: ret;
	}

	return read;
}

static void virtio_cleanup(struct hwrng *rng)
{
	struct virtrng_info *vi = (struct virtrng_info *)rng->priv;

	WRITE_ONCE(vi->cleanup_pending, true);
	complete(&vi->have_data);
}

static int probe_common(struct virtio_device *vdev)
{
	int err, index;
	struct virtrng_info *vi = NULL;

	vi = kzalloc_obj(struct virtrng_info);
	if (!vi)
		return -ENOMEM;

	index = ida_alloc(&rng_index_ida, GFP_KERNEL);
	vi->index = index;
	if (index < 0) {
		err = index;
		goto err_ida;
	}
	sprintf(vi->name, "virtio_rng.%d", index);
	init_completion(&vi->have_data);
	mutex_init(&vi->process_lock);
	INIT_DELAYED_WORK(&vi->refill_work, refill_entropy);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	INIT_WORK(&vi->publication_work, publish_held_data);
#endif

	vi->hwrng = (struct hwrng) {
		.read = virtio_read,
		.cleanup = virtio_cleanup,
		.priv = (unsigned long)vi,
		.name = vi->name,
	};
	vdev->priv = vi;

	/* We expect a single virtqueue. */
	vi->vq = virtio_find_single_vq(vdev, random_recv_done, "input");
	if (IS_ERR(vi->vq)) {
		err = PTR_ERR(vi->vq);
		goto err_find;
	}

	virtio_device_ready(vdev);

	/* we always have a pending entropy request */
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	err = vrng_shadow_init(&vi->shadow, sizeof(vi->data));
	if (err)
		goto err_request;
	vi->shadow_initialized = true;
	vi->shadow_active = true;
	err = vrng_driver_shadow_init(&vi->driver_shadow);
	if (err)
		goto err_request;
	vi->driver_shadow_initialized = true;
	mutex_lock(&vrng_teardown_record_lock);
	vi->evidence_cookie = ++vrng_next_device_cookie;
	mutex_unlock(&vrng_teardown_record_lock);
	WRITE_ONCE(vrng_lang_completion_held, 0);
	WRITE_ONCE(vrng_lang_publication_held, 0);
	WRITE_ONCE(vrng_lang_remove_begun, 0);
	WRITE_ONCE(vrng_lang_last_remove_data_avail, -1);
	WRITE_ONCE(vrng_lang_last_remove_lifecycle, -1);
	WRITE_ONCE(vrng_lang_last_remove_phase, -1);
#endif
	mutex_lock(&vi->process_lock);
	err = request_entropy_locked(vi);
	mutex_unlock(&vi->process_lock);
	if (err)
		goto err_request;

	return 0;

err_request:
	{
		int teardown_err = remove_common(vdev);

		if (teardown_err)
			return teardown_err;
	}
	return err;

err_find:
	vdev->priv = NULL;
	ida_free(&rng_index_ida, index);
err_ida:
	kfree(vi);
	return err;
}

static int remove_common(struct virtio_device *vdev)
{
	struct virtrng_info *vi = vdev->priv;
	bool unregister;
	int first_err = 0;

	if (!vi)
		return 0;
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	struct vrng_shadow_mismatch last;
	struct vrng_core_state final_state;
	struct vrng_driver_state final_driver_state;
	u64 events, mismatches, driver_events = 0, driver_mismatches = 0;
	int err;

	mutex_lock(&vi->process_lock);
	if (vi->shadow_active) {
		err = vrng_shadow_begin_remove(&vi->shadow);
		virtrng_record_first_error(&first_err, err);
	}
	if (vi->driver_shadow_initialized) {
		bool model_unregister = false;

		err = vrng_driver_shadow_begin_remove(&vi->driver_shadow,
						      &model_unregister);
		virtrng_record_first_error(&first_err, err);
		if (!err && model_unregister != vi->hwrng_register_done)
			virtrng_record_first_error(&first_err, -EPROTO);
	}
#else
	mutex_lock(&vi->process_lock);
#endif

	vi->hwrng_removed = true;
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	WRITE_ONCE(vrng_lang_remove_begun, 1);
#endif
	unregister = vi->hwrng_register_done;
	vi->hwrng_register_done = false;
	mutex_unlock(&vi->process_lock);
	complete(&vi->have_data);
	cancel_delayed_work_sync(&vi->refill_work);
	if (unregister)
		hwrng_unregister(&vi->hwrng);
	virtio_reset_device(vdev);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	/* Reset prevents callbacks from scheduling a new publication worker. */
	cancel_work_sync(&vi->publication_work);
#endif
	vdev->config->del_vqs(vdev);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	if (vi->driver_shadow_initialized) {
		err = vrng_driver_shadow_drain(&vi->driver_shadow);
		virtrng_record_first_error(&first_err, err);
		err = vrng_driver_shadow_final_clear(&vi->driver_shadow);
		virtrng_record_first_error(&first_err, err);
	}
#endif
	/* No callback can republish data after the final externally visible clear. */
	mutex_lock(&vi->process_lock);
	WRITE_ONCE(vi->data_avail, 0);
	vi->data_idx = 0;
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	if (vi->shadow_active) {
		err = vrng_shadow_finish_remove(&vi->shadow);
		virtrng_record_first_error(&first_err, err);
		vi->shadow_active = false;
		vrng_shadow_control_snapshot(&vi->shadow, &final_state);
		WRITE_ONCE(vrng_lang_last_remove_lifecycle,
			   final_state.lifecycle);
		WRITE_ONCE(vrng_lang_last_remove_phase, final_state.phase);
	}
	if (vi->driver_shadow_initialized) {
		err = vrng_driver_shadow_finish_remove(&vi->driver_shadow);
		virtrng_record_first_error(&first_err, err);
	}
	WRITE_ONCE(vrng_lang_last_remove_data_avail,
		   READ_ONCE(vi->data_avail));
#endif
	mutex_unlock(&vi->process_lock);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	if (!vi->shadow_initialized)
		goto free_info;
	vrng_shadow_snapshot(&vi->shadow, &events, &mismatches, &last);
	if (vi->driver_shadow_initialized)
		vrng_driver_shadow_snapshot(&vi->driver_shadow, &driver_events,
					    &driver_mismatches,
					    &final_driver_state);
	if (xchg(&vrng_lang_fail_teardown_once, 0))
		virtrng_record_first_error(&first_err, -EPROTO);
	mutex_lock(&vrng_teardown_record_lock);
	vrng_last_teardown_record.sequence++;
	vrng_last_teardown_record.device_cookie = vi->evidence_cookie;
	vrng_last_teardown_record.teardown_error = first_err;
	vrng_last_teardown_record.events = driver_events;
	vrng_last_teardown_record.mismatches = driver_mismatches;
	if (vi->driver_shadow_initialized) {
		vrng_last_teardown_record.stage = final_driver_state.stage;
		vrng_last_teardown_record.external_avail =
			final_driver_state.external_avail;
	} else {
		vrng_last_teardown_record.stage = -1;
		vrng_last_teardown_record.external_avail = -1;
	}
	mutex_unlock(&vrng_teardown_record_lock);
	if (mismatches || driver_mismatches)
		dev_warn(&vdev->dev,
			 "language shadow control=%s mismatches=%llu events=%llu driver_mismatches=%llu driver_events=%llu teardown_error=%d last_event=%u last_sequence=%llu C=%d Rust=%d MC=%d spec=%d\n",
			 vrng_shadow_control_name(),
			 mismatches, events, driver_mismatches, driver_events,
			 first_err, last.event, last.sequence,
			 last.c_result, last.rust_result, last.mc_result,
			 last.spec_result);
	else if (first_err)
		dev_warn(&vdev->dev,
			 "language shadow control=%s teardown failed=%d after %llu protocol and %llu driver lifecycle events\n",
			 vrng_shadow_control_name(), first_err, events,
			 driver_events);
	else
		dev_info(&vdev->dev,
			 "language shadow control=%s matched all %llu protocol and %llu driver lifecycle events\n",
			 vrng_shadow_control_name(), events, driver_events);
free_info:
#endif
	ida_free(&rng_index_ida, vi->index);
	vdev->priv = NULL;
	kfree(vi);
	return first_err;
}

static int virtrng_probe(struct virtio_device *vdev)
{
	return probe_common(vdev);
}

static void virtrng_remove(struct virtio_device *vdev)
{
	remove_common(vdev);
}

static int virtrng_hwrng_register(struct virtrng_info *vi)
{
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	if (xchg(&vrng_lang_fail_register_once, 0))
		return -EIO;
#endif
	return hwrng_register(&vi->hwrng);
}

static void virtrng_scan(struct virtio_device *vdev)
{
	struct virtrng_info *vi = vdev->priv;
	int err;

	err = virtrng_hwrng_register(vi);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	{
		int lifecycle_err;

		lifecycle_err =
			vrng_driver_shadow_register(&vi->driver_shadow, !err);
		if (lifecycle_err) {
			if (!err)
				hwrng_unregister(&vi->hwrng);
			publish_fatal_error(vi, lifecycle_err);
			dev_err(&vdev->dev,
				"language driver lifecycle rejected registration: %d\n",
				lifecycle_err);
			return;
		}
	}
#endif
	if (!err) {
		mutex_lock(&vi->process_lock);
		vi->hwrng_register_done = true;
		mutex_unlock(&vi->process_lock);
	} else {
		dev_err(&vdev->dev,
			"hwrng registration failed: %d; device remains bound but unavailable\n",
			err);
	}
}

static int virtrng_freeze(struct virtio_device *vdev)
{
	/*
	 * Always finish physical cleanup, but propagate logical verification
	 * failure.
	 */
	return remove_common(vdev);
}

static int virtrng_restore(struct virtio_device *vdev)
{
	int err;

	err = probe_common(vdev);
	if (!err) {
		struct virtrng_info *vi = vdev->priv;

		/*
		 * Set hwrng_removed to ensure that virtio_read()
		 * does not block waiting for data before the
		 * registration is complete.
		 */
		mutex_lock(&vi->process_lock);
		vi->hwrng_removed = true;
		mutex_unlock(&vi->process_lock);
		err = virtrng_hwrng_register(vi);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
		if (vi->driver_shadow_initialized) {
			int lifecycle_err;

			lifecycle_err =
				vrng_driver_shadow_register(&vi->driver_shadow,
							    !err);
			if (lifecycle_err) {
				if (!err)
					hwrng_unregister(&vi->hwrng);
				err = lifecycle_err;
			}
		}
#endif
		if (!err) {
			mutex_lock(&vi->process_lock);
			vi->hwrng_register_done = true;
			vi->hwrng_removed = false;
			mutex_unlock(&vi->process_lock);
		} else {
			int teardown_err = remove_common(vdev);

			if (teardown_err)
				err = teardown_err;
		}
	}

	return err;
}

static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_RNG, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_rng_driver = {
	.driver.name =	KBUILD_MODNAME,
	.id_table =	id_table,
	.probe =	virtrng_probe,
	.remove =	virtrng_remove,
	.scan =		virtrng_scan,
	.freeze =	pm_sleep_ptr(virtrng_freeze),
	.restore =	pm_sleep_ptr(virtrng_restore),
};

module_virtio_driver(virtio_rng_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio random number driver");
MODULE_LICENSE("GPL");
