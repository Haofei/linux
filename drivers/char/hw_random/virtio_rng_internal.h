/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _VIRTIO_RNG_INTERNAL_H
#define _VIRTIO_RNG_INTERNAL_H

#include <linux/errno.h>
#include <linux/atomic.h>
#include <linux/minmax.h>
#include <linux/nospec.h>
#include <linux/string.h>
#include <linux/types.h>

static inline int virtrng_copy_available(const u8 *source, u32 capacity,
					 u32 *index, u32 *remaining,
					 void *destination, u32 requested)
{
	u32 amount, hardened_index;

	if (*index > capacity || *remaining > capacity - *index)
		return -EOVERFLOW;

	amount = min(requested, *remaining);
	if (!amount)
		return 0;

	hardened_index = array_index_nospec(*index, capacity);
	memcpy(destination, source + hardened_index, amount);
	*index += amount;
	*remaining -= amount;
	return amount;
}

static inline int virtrng_read_error(int fatal_errno, int *transient_errno)
{
	if (fatal_errno)
		return fatal_errno;

	return xchg(transient_errno, 0);
}

#endif /* _VIRTIO_RNG_INTERNAL_H */
