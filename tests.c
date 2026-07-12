#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "minunit.h"
#include "rbtree.c"

#include "opt.c"
#include "util.c"
#include "debug.c"
#include "csum.c"
#include "threads.c"
#include "btrfs-util.c"
#include "file_scan.c"
#include "filerec.c"
#include "dbfile.c"
#include "hash-tree.c"
#include "results-tree.c"
#include "list_sort.c"
#include "find_dupes.c"
#include "memstats.c"
#include "fiemap.c"
#include "progress.c"


unsigned int blocksize = DEFAULT_BLOCKSIZE;
static char *exec_path;

static struct fiemap *alloc_test_fiemap(unsigned int count, uint64_t file_size)
{
	struct fiemap *fiemap;

	fiemap = calloc(1, sizeof(*fiemap) +
			count * sizeof(struct fiemap_extent));
	abort_on(!fiemap);

	fiemap->fm_mapped_extents = count;
	fiemap->fm_extent_count = count;
	fiemap->fm_length = file_size;
	return fiemap;
}

MU_TEST(test_is_block_zeroed) {
	blocksize = 100;
	char block[100] = {0,};
	// Actual zeroed block
	mu_check(is_block_zeroed(&block) == true);

	// Block has the same content, but not zeroed
	memset(block, 1, 100);
	mu_check(is_block_zeroed(&block) == false);

	// Block do not have the same content
	block[50] = 50;
	mu_check(is_block_zeroed(NULL) == false);
}

MU_TEST(test_block_len) {
	struct file_block block;
	struct filerec file;

	block.b_file = &file;

	// First block of the file
	file.size = 10 * 1024 * 1024;
	block.b_loff = 0;
	mu_check(block_len(&block) == blocksize);

	// block in the middle of the file, unaligned
	block.b_loff = 1;
	mu_check(block_len(&block) == blocksize);

	// block in the middle of the file, aligned
	block.b_loff = blocksize * 10;
	mu_check(block_len(&block) == blocksize);

	// block at the end of the file, which is aligned
	file.size = blocksize * 10;
	block.b_loff = blocksize * 9;
	mu_check(block_len(&block) == blocksize);

	// block at the end of the file, which is unaligned
	unsigned int extra = 10;
	file.size = blocksize * 10 + extra;
	block.b_loff = blocksize * 10;
	mu_check(block_len(&block) == extra);

	// loff is passed filesize
	file.size = blocksize * 10 + extra;
	block.b_loff = blocksize * 15;
	mu_check(block_len(&block) == 0);
}

MU_TEST(test_is_file_renamed) {
	char *new_path = "/tmp/somefile";
	char *path_in_db = "/tmp/somefile";

	mu_check(is_file_renamed(path_in_db, new_path) == false);

	path_in_db = "/tmp/anotherfile";
	mu_check(is_file_renamed(path_in_db, new_path) == true);

	/*
	 * Diffents path but the old one still exists.
	 * We use our own file to simulate a hard link
	 */
	mu_check(is_file_renamed(exec_path, new_path) == false);
}

MU_TEST(test_get_extent_sparse_holes) {
	struct fiemap *fiemap = alloc_test_fiemap(2, 64 * 1024);
	struct fiemap_extent *extent;
	unsigned int index;

	fiemap->fm_extents[0].fe_logical = 4 * 1024;
	fiemap->fm_extents[0].fe_length = 4 * 1024;
	fiemap->fm_extents[1].fe_logical = 16 * 1024;
	fiemap->fm_extents[1].fe_length = 4 * 1024;
	fiemap->fm_extents[1].fe_flags = FIEMAP_EXTENT_LAST;

	/* Leading hole. */
	extent = get_extent(fiemap, 0, &index);
	mu_check(index == 0);
	mu_check(extent->fe_logical == 0);
	mu_check(extent->fe_length == 4 * 1024);
	mu_check(extent->fe_flags & FIEMAP_EXTENT_UNWRITTEN);
	mu_check(!(extent->fe_flags & FIEMAP_EXTENT_LAST));

	/* First mapped extent. */
	extent = get_extent(fiemap, 4 * 1024, &index);
	mu_check(index == 0);
	mu_check(extent == &fiemap->fm_extents[0]);

	/* Inner hole. */
	extent = get_extent(fiemap, 8 * 1024, &index);
	mu_check(index == 1);
	mu_check(extent->fe_logical == 8 * 1024);
	mu_check(extent->fe_length == 8 * 1024);
	mu_check(extent->fe_flags & FIEMAP_EXTENT_UNWRITTEN);

	/* Trailing hole. */
	extent = get_extent(fiemap, 20 * 1024, &index);
	mu_check(index == 2);
	mu_check(extent->fe_logical == 20 * 1024);
	mu_check(extent->fe_length == 44 * 1024);
	mu_check(extent->fe_flags & FIEMAP_EXTENT_UNWRITTEN);
	mu_check(extent->fe_flags & FIEMAP_EXTENT_LAST);

	mu_check(get_extent(fiemap, 64 * 1024, NULL) == NULL);
	free(fiemap);
}

MU_TEST(test_get_extent_fully_sparse_file) {
	struct fiemap *fiemap = alloc_test_fiemap(0, 32 * 1024);
	struct fiemap_extent *extent;

	extent = get_extent(fiemap, 0, NULL);
	mu_check(extent != NULL);
	mu_check(extent->fe_logical == 0);
	mu_check(extent->fe_length == 32 * 1024);
	mu_check(extent->fe_flags & FIEMAP_EXTENT_UNWRITTEN);
	mu_check(extent->fe_flags & FIEMAP_EXTENT_LAST);

	free(fiemap);
}

MU_TEST(test_clamp_fiemap_extents_to_eof) {
	struct fiemap *fiemap = alloc_test_fiemap(3, 10 * 1024);

	fiemap->fm_extents[0].fe_logical = 0;
	fiemap->fm_extents[0].fe_length = 4 * 1024;
	fiemap->fm_extents[1].fe_logical = 8 * 1024;
	fiemap->fm_extents[1].fe_length = 4 * 1024;
	fiemap->fm_extents[2].fe_logical = 12 * 1024;
	fiemap->fm_extents[2].fe_length = 4 * 1024;

	clamp_fiemap_extents_to_eof(fiemap, 10 * 1024);

	mu_check(fiemap->fm_length == 10 * 1024);
	mu_check(fiemap->fm_mapped_extents == 2);
	mu_check(fiemap->fm_extents[0].fe_length == 4 * 1024);
	mu_check(fiemap->fm_extents[1].fe_logical == 8 * 1024);
	mu_check(fiemap->fm_extents[1].fe_length == 2 * 1024);
	mu_check(fiemap->fm_extents[1].fe_flags & FIEMAP_EXTENT_LAST);

	free(fiemap);
}

MU_TEST_SUITE(test_suite) {
	MU_RUN_TEST(test_is_block_zeroed);
	MU_RUN_TEST(test_block_len);
	MU_RUN_TEST(test_is_file_renamed);
	MU_RUN_TEST(test_get_extent_sparse_holes);
	MU_RUN_TEST(test_get_extent_fully_sparse_file);
	MU_RUN_TEST(test_clamp_fiemap_extents_to_eof);
}

int main(int argc [[maybe_unused]], char *argv[]) {
	exec_path = argv[0];
	MU_RUN_SUITE(test_suite);
	MU_REPORT();
	return MU_EXIT_CODE;
}
