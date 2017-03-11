#include <linux/buffer_head.h>
#include <linux/slab.h>

#include "device.h"

size_t tarfs_read(void *buffer, size_t size, off_t offset,
                  struct super_block *sb)
{
  struct buffer_head *bh;
  size_t pos = 0;

  while (size > 0) {
    unsigned int block = (offset + pos) / sb->s_blocksize;
    off_t inner_off = (offset + pos) % sb->s_blocksize;
    size_t inner_size = min_t(size_t, sb->s_blocksize - inner_off, size);

    bh = sb_bread(sb, block);

    if (!bh)
      pr_err("tarfs: Failed to read block %u", block);

    if (bh->b_size != sb->s_blocksize)
      pr_err("tarfs: Wanted %lu byte block, but got %lu", sb->s_blocksize, bh->b_size);

    memcpy(buffer, bh->b_data + inner_off, inner_size);
    brelse(bh);

    pos += inner_size;
    size -= inner_size;
    buffer += inner_size;
  }

  return pos;
}
