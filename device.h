#ifndef DEVICE_H
#define DEVICE_H

#include <linux/types.h>
#include <linux/fs.h>

/**
 * @brief reads part of the block device into \a baffer
 * @param buffer the destination buffer
 * @param size the buffer size
 * @param offset byte offset in the block device relative to its start
 * @param sb the super block descriptor
 * @return count of bytes read
 */
 size_t tarfs_read(void *buffer, size_t size, off_t offset,
                   struct super_block *sb);

#endif
