#ifndef TAR_H
#define TAR_H

#include <linux/uidgid.h>
#include <linux/time.h>
#include <linux/types.h>
#include "gnutar.h"

/** Describes access to an entry in a tar file. */
struct tar_entry {
  /** The entry header. */
  struct star_header header;

  /** Path to the entry. */
  char *dirname;

  /** Name of the entry. */
  char *basename;

  /** File offset to the entry header. */
  unsigned int offset;

  /** File offset to the entry data. */
  unsigned int data_offset;

  /** Length of the data. */
  size_t length;

  /** The inode number of the entry. */
  unsigned long inode;

  /** Mode flags. */
  umode_t mode;

  /** User id */
  uid_t uid;

  /** Group id */
  gid_t gid;

  /** UNIX timestamp of last access. */
  struct timespec atime;

  /** UNIX timestamp of last modification. */
  struct timespec mtime;

  /** UNIX timestamp of creation. */
  struct timespec ctime;

  /** Next entry, or \c NULL if none. */
  struct tar_entry *next;
};

/**
 * @brief Maps a tar file type to a linux file type.
 * @param typeflag the type value
 * @return the linux file type
 */
mode_t tar_type_to_posix(int typeflag);

/**
 * @brief Reads a TAR entry
 * @param sb the super block to read from
 * @param offset the read offset
 * @return the read entry, or \c NULL if reading failed
 */
struct tar_entry *tar_read_entry(struct super_block *sb, off_t offset);

/**
 * @brief Reads all tar entries from \a handle into a linked list.
 * @param sb the super block to read from
 * @return the found entries
 * @note Does \b not lock
 */
struct tar_entry *tar_open(struct super_block *sb);

/**
 * @brief Frees memory allocated by \a entry and all following entries.
 * @param entry the entry to free
 */
void tar_free(struct tar_entry *entry);

/**
 * @brief Reads data of \a entry from \a handle starting at \a off into
 *        \a buffer.
 * @param sb the super block to read from
 * @param entry the entry to read
 * @param off offset in the data
 * @param buffer the target buffer
 * @param len maximum count of bytes to read
 * @return the count of bytes read
 * @note Does \b not lock
 */
size_t tar_read(struct super_block *sb, struct tar_entry *entry,
                unsigned int off, void *buffer, size_t len);

/**
* @brief Finds an entry starting at \a entry by its \a dirname and \a basename.
* @param entry the start entry
* @param dirname the directory path name of the entry
* @param basename the base name of the entry
* @return the found entry or \c NULL
*/
struct tar_entry *tar_find(struct tar_entry *entry, const char *dirname,
                           const char *basename);

#endif
