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
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
#include "virtio_rng_lang/vrng_shadow.h"
#endif

static DEFINE_IDA(rng_index_ida);

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
static int vrng_lang_fail_add_once;
static int vrng_lang_completion_override = -1;
static int vrng_lang_stale_once;
module_param_named(lang_fail_add_once, vrng_lang_fail_add_once, int, 0600);
module_param_named(lang_completion_override, vrng_lang_completion_override,
		   int, 0600);
module_param_named(lang_stale_once, vrng_lang_stale_once, int, 0600);
MODULE_PARM_DESC(lang_fail_add_once,
		 "fail the next experimental virtqueue submission");
MODULE_PARM_DESC(lang_completion_override,
		 "replace the next experimental completion length (-1 disables)");
MODULE_PARM_DESC(lang_stale_once,
		 "use a stale generation for the next experimental completion");
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
#endif
};

static int request_entropy_locked(struct virtrng_info *vi);

static void publish_data_error(struct virtrng_info *vi, int error)
{
	WRITE_ONCE(vi->data_error, error);
	/* Publish the error before waking readers that acquire data_avail. */
	smp_store_release(&vi->data_avail, 0);
	complete(&vi->have_data);
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
		publish_data_error(vi, err);
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
	int err;
#endif

	/* We can get spurious callbacks, e.g. shared IRQs + virtio_pci. */
	cookie = virtqueue_get_buf(vi->vq, &len);
	if (!cookie)
		return;

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	if (WARN_ON_ONCE(cookie != &vi->cookie || cookie->vi != vi))
		return;
	if (READ_ONCE(vrng_lang_completion_override) >= 0)
		len = xchg(&vrng_lang_completion_override, -1);
	generation = cookie->generation;
	if (xchg(&vrng_lang_stale_once, 0))
		generation--;
	err = vrng_shadow_complete(&vi->shadow, generation, len, NULL);
	if (err) {
		if (err == -ESTALE)
			vrng_shadow_recover_consumed(&vi->shadow);
		publish_data_error(vi, err);
		return;
	}
#else
	if (!len || len > sizeof(vi->data)) {
		publish_data_error(vi, len ? -EOVERFLOW : -ENODATA);
		return;
	}
#endif
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

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	err = vrng_shadow_begin_submit(&vi->shadow, &generation);
	if (err)
		return err;
	vi->cookie.vi = vi;
	vi->cookie.epoch = vi->shadow.c_state.epoch;
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
		vrng_shadow_abort_submit(&vi->shadow, generation);
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

#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	u32 copied = 0, need_resubmit = 0;

	err = vrng_shadow_copy(&vi->shadow, vi->data, buf, size, &copied,
			       &need_resubmit);
	if (err)
		return err;
	vi->data_idx += copied;
	vi->data_avail -= copied;
	if (need_resubmit) {
		err = request_entropy_locked(vi);
		if (err)
			publish_data_error(vi, err);
	}
	return copied;
#else
	unsigned int idx, avail;

	lockdep_assert_held(&vi->process_lock);

	/*
	 * vi->data_avail was set from the device-reported used.len and
	 * vi->data_idx was advanced by previous copy_data() calls.  A
	 * malicious or buggy virtio-rng backend can drive either past
	 * sizeof(vi->data).  Clamp at point of use and harden the index
	 * with array_index_nospec() so the memcpy() below cannot be
	 * steered into adjacent slab memory, including under
	 * speculation.
	 */
	avail = min_t(unsigned int, vi->data_avail, sizeof(vi->data));
	if (vi->data_idx >= avail) {
		vi->data_avail = 0;
		err = request_entropy_locked(vi);
		return err ?: 0;
	}
	size = min_t(unsigned int, size, avail - vi->data_idx);
	idx = array_index_nospec(vi->data_idx, sizeof(vi->data));
	memcpy(buf, vi->data + idx, size);
	vi->data_idx += size;
	vi->data_avail -= size;
	if (vi->data_avail == 0) {
		err = request_entropy_locked(vi);
		if (err)
			publish_data_error(vi, err);
	}
	return size;
#endif
}

static int virtio_read(struct hwrng *rng, void *buf, size_t size, bool wait)
{
	int ret;
	struct virtrng_info *vi = (struct virtrng_info *)rng->priv;
	int chunk, pending_error;
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
		pending_error = xchg(&vi->data_error, 0);
		if (pending_error) {
			mod_delayed_work(system_wq, &vi->refill_work, 0);
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

	vi->index = index = ida_alloc(&rng_index_ida, GFP_KERNEL);
	if (index < 0) {
		err = index;
		goto err_ida;
	}
	sprintf(vi->name, "virtio_rng.%d", index);
	init_completion(&vi->have_data);
	mutex_init(&vi->process_lock);
	INIT_DELAYED_WORK(&vi->refill_work, refill_entropy);

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
#endif
	mutex_lock(&vi->process_lock);
	err = request_entropy_locked(vi);
	mutex_unlock(&vi->process_lock);
	if (err)
		goto err_request;

	return 0;

err_request:
	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);

err_find:
	ida_free(&rng_index_ida, index);
err_ida:
	kfree(vi);
	return err;
}

static void remove_common(struct virtio_device *vdev)
{
	struct virtrng_info *vi = vdev->priv;
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	struct vrng_shadow_mismatch last;
	u64 events, mismatches;

	mutex_lock(&vi->process_lock);
	vrng_shadow_begin_remove(&vi->shadow);
#else
	mutex_lock(&vi->process_lock);
#endif

	vi->hwrng_removed = true;
	vi->data_avail = 0;
	vi->data_idx = 0;
	mutex_unlock(&vi->process_lock);
	complete(&vi->have_data);
	cancel_delayed_work_sync(&vi->refill_work);
	if (vi->hwrng_register_done)
		hwrng_unregister(&vi->hwrng);
	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);
#if IS_ENABLED(CONFIG_HW_RANDOM_VIRTIO_LANG_SHADOW)
	mutex_lock(&vi->process_lock);
	vrng_shadow_finish_remove(&vi->shadow);
	mutex_unlock(&vi->process_lock);
	vrng_shadow_snapshot(&vi->shadow, &events, &mismatches, &last);
	if (mismatches)
		dev_warn(&vdev->dev,
			 "language shadow mismatches=%llu events=%llu last_event=%u last_sequence=%llu C=%d Rust=%d MC=%d\n",
			 mismatches, events, last.event, last.sequence,
			 last.c_result, last.rust_result, last.mc_result);
	else
		dev_info(&vdev->dev,
			 "language shadow matched all %llu protocol events\n",
			 events);
#endif
	ida_free(&rng_index_ida, vi->index);
	kfree(vi);
}

static int virtrng_probe(struct virtio_device *vdev)
{
	return probe_common(vdev);
}

static void virtrng_remove(struct virtio_device *vdev)
{
	remove_common(vdev);
}

static void virtrng_scan(struct virtio_device *vdev)
{
	struct virtrng_info *vi = vdev->priv;
	int err;

	err = hwrng_register(&vi->hwrng);
	if (!err)
		vi->hwrng_register_done = true;
}

static int virtrng_freeze(struct virtio_device *vdev)
{
	remove_common(vdev);
	return 0;
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
		vi->hwrng_removed = true;
		err = hwrng_register(&vi->hwrng);
		if (!err) {
			vi->hwrng_register_done = true;
			vi->hwrng_removed = false;
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
