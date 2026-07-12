#ifndef	__FIEMAP_H__
#define	__FIEMAP_H__

#include <linux/fiemap.h>
#include <sys/types.h>
#include <stdint.h>

/*
 * Given a filled fiemap structure, extract the struct fiemap_extent which
 * covers loff.
 *
 * Sparse holes have no kernel FIEMAP record. For those ranges this returns a
 * thread-local synthetic extent flagged FIEMAP_EXTENT_UNWRITTEN, allowing
 * callers to advance over the hole without treating it as a file-changing
 * race. The synthetic value must be consumed before the next get_extent()
 * call in the same thread.
 *
 * If index is not NULL, it receives the real extent index or the insertion
 * position of a synthetic hole. If loff is at or beyond the logical EOF,
 * returns NULL and index is garbage.
 *
 * The returned value must not be used after fiemap is freed, and must not be
 * freed directly either.
 */
struct fiemap_extent *get_extent(struct fiemap *fiemap, size_t loff,
				 unsigned int *index);

/*
 * Extract the extent mapping of a file. Reported extent lengths are clamped
 * to the logical EOF so downstream FIDEDUPERANGE requests cannot cross it.
 *
 * May not return all extents if the file changed while this function was
 * running.
 */
struct fiemap *do_fiemap(int fd);

/*
 * Count how much of the area between start_off and end_off is shared.
 */
int fiemap_count_shared(int fd, size_t start_off, size_t end_off, uint64_t *shared);
#endif	/* __FIEMAP_H__ */
