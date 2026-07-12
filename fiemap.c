/*
 * fiemap.c
 *
 * Abstract and add helpers to the fiemap ioctl.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>

#include "debug.h"
#include "fiemap.h"
#include "util.h"

/*
 * Invoke an empty fiemap ioctl to fetch the number of extents for this file.
 * Returns 0 on error.
 */
static unsigned int fiemap_count_extents(int fd, uint64_t file_size)
{
	struct fiemap fiemap = {0,};
	int err;

	if (file_size == 0)
		return 0;

	fiemap.fm_start = 0;
	fiemap.fm_length = file_size;

	err = ioctl(fd, FS_IOC_FIEMAP, &fiemap);
	if (err < 0) {
		perror("fiemap_count_extents");
		return 0;
	}

	return fiemap.fm_mapped_extents;
}

/*
 * FIEMAP does not return records for sparse holes. Represent those holes as
 * synthetic unwritten extents so callers can advance over them without
 * confusing a valid sparse range with a file-changing race.
 *
 * The result is thread-local because get_extent() is used by scan workers in
 * parallel. Callers must consume it before their next get_extent() call.
 */
static __thread struct fiemap_extent sparse_hole;

static struct fiemap_extent *make_sparse_hole(uint64_t start, uint64_t end,
					       bool last)
{
	abort_on(start >= end);

	memset(&sparse_hole, 0, sizeof(sparse_hole));
	sparse_hole.fe_logical = start;
	sparse_hole.fe_length = end - start;
	sparse_hole.fe_flags = FIEMAP_EXTENT_UNWRITTEN;
	if (last)
		sparse_hole.fe_flags |= FIEMAP_EXTENT_LAST;

	return &sparse_hole;
}

/*
 * Restrict FIEMAP's block-aligned extent lengths to the logical file size.
 * Btrfs commonly reports the final extent rounded up to the filesystem block
 * size. Passing that rounded length to FIDEDUPERANGE crosses EOF and is
 * rejected with EINVAL.
 */
static void clamp_fiemap_extents_to_eof(struct fiemap *fiemap,
					uint64_t file_size)
{
	unsigned int out = 0;

	for (unsigned int i = 0; i < fiemap->fm_mapped_extents; i++) {
		struct fiemap_extent extent = fiemap->fm_extents[i];

		if (extent.fe_logical >= file_size || extent.fe_length == 0)
			continue;

		if (extent.fe_length > file_size - extent.fe_logical)
			extent.fe_length = file_size - extent.fe_logical;

		fiemap->fm_extents[out++] = extent;
	}

	fiemap->fm_mapped_extents = out;
	fiemap->fm_length = file_size;

	if (out)
		fiemap->fm_extents[out - 1].fe_flags |= FIEMAP_EXTENT_LAST;
}

struct fiemap_extent *get_extent(struct fiemap *fiemap, size_t loff,
				 unsigned int *index)
{
	uint64_t logical = loff;

	for (unsigned int i = 0; i < fiemap->fm_mapped_extents; i++) {
		struct fiemap_extent *extent = &fiemap->fm_extents[i];
		uint64_t start = extent->fe_logical;
		uint64_t end = start + extent->fe_length;

		if (logical < start) {
			if (index)
				*index = i;
			return make_sparse_hole(logical, start, false);
		}

		if (logical < end) {
			if (index)
				*index = i;
			return extent;
		}
	}

	if (logical < fiemap->fm_length) {
		if (index)
			*index = fiemap->fm_mapped_extents;
		return make_sparse_hole(logical, fiemap->fm_length, true);
	}

	return NULL;
}

struct fiemap *do_fiemap(int fd)
{
	int err;
	struct stat st;
	uint64_t file_size;

	struct fiemap *fiemap = NULL;
	unsigned int count;

	err = fstat(fd, &st);
	if (err < 0) {
		perror("fstat");
		return NULL;
	}
	file_size = st.st_size;
	count = fiemap_count_extents(fd, file_size);

	/*
	 * Our structure must be large enough to fit:
	 * - one struct fiemap = 32 bytes
	 * - $count struct fiemap_extent = count * 56 bytes
	 * - $count struct fiemap_extent* = count * 4 bytes
	 * See https://www.kernel.org/doc/Documentation/filesystems/fiemap.txt
	 */
	fiemap = calloc(1, sizeof(struct fiemap) +
			count * (sizeof(struct fiemap_extent) +
			sizeof(struct fiemap_extent *)));
	if (!fiemap)
		return NULL;

	fiemap->fm_start = 0;
	fiemap->fm_length = file_size;
	fiemap->fm_extent_count = count;

	if (file_size == 0)
		return fiemap;

	err = ioctl(fd, FS_IOC_FIEMAP, fiemap);
	if (err < 0) {
		perror("fiemap");
		free(fiemap);
		return NULL;
	}

	if (fiemap->fm_mapped_extents != count)
		dprintf("do_fiemap: file changed between fiemap calls\n");

	clamp_fiemap_extents_to_eof(fiemap, file_size);
	return fiemap;
}

int fiemap_count_shared(int fd, size_t start_off, size_t end_off, uint64_t *shared)
{
	_cleanup_(freep) struct fiemap *fiemap = NULL;
	struct fiemap_extent *extent;

	size_t extent_loff;
	size_t extent_end;

	abort_on(start_off >= end_off);

	fiemap = do_fiemap(fd);
	if (!fiemap)
		return 1;

	*shared = 0;

	for (unsigned int i = 0; i < fiemap->fm_mapped_extents; i++) {
		extent = &fiemap->fm_extents[i];

		extent_end = extent->fe_logical + extent->fe_length;
		extent_loff = extent->fe_logical;

		if (start_off <= extent_end && end_off >= extent_loff) {
			if (!(extent->fe_flags & FIEMAP_EXTENT_DELALLOC)
					&& extent->fe_flags & FIEMAP_EXTENT_SHARED) {
				if (extent_loff < start_off)
					extent_loff = start_off;
				if (end_off < extent_end)
					extent_end = end_off;
				*shared += extent_end - extent_loff;
			}
		}
	}
	return 0;
}
