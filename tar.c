#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/fs.h>

#include "tar.h"
#include "device.h"

/** Aligns \a x on a 512 boundary. */
#define ALIGN_SECTOR(x) (((x) % 512 > 0) ? 512 - ((x) % 512) : 0)

#define OCTAL (8)

mode_t tar_type_to_posix(int typeflag)
{
  switch(typeflag) {
    case REGTYPE:
    case AREGTYPE:
      return S_IFREG;
    case DIRTYPE:
      return S_IFDIR;
    case SYMTYPE:
      return S_IFLNK;
    case CHRTYPE:
      return S_IFCHR;
    case BLKTYPE:
      return S_IFBLK;
    case FIFOTYPE:
      return S_IFIFO;
    default:
      return 0;
  }
}

/**
 * @brief Allocates a string by combining prefix and name from \a header.
 * @return the string, must be free'd later by the caller.
 */
static char *build_name(struct star_header *header)
{
  char *prefix_end = memchr(header->prefix, 0, sizeof(header->prefix));
  char *name_end = memchr(header->name, 0, sizeof(header->name));

  size_t prefix_len = prefix_end - header->prefix;
  size_t name_len = name_end - header->name;

  char *name = kmalloc(prefix_len + name_len + 1, GFP_KERNEL);

  if (!name)
    return NULL;

  memcpy(name, header->prefix, prefix_len);
  memcpy(name + prefix_len, header->name, name_len);
  name[prefix_len + name_len] = 0x0;

  // The path name ends with a slash if the entry is a directory
  if (name[prefix_len + name_len - 1] == '/')
    name[prefix_len + name_len - 1] = 0x0;

  return name;
}

struct tar_entry *tar_read_entry(struct super_block *sb, off_t offset)
{
  struct star_header header;
  char *full_name = NULL;
  char *basename = NULL;
  struct tar_entry *entry = NULL;
  unsigned int length = 0;
  unsigned int mode = 0;
  uid_t uid = 0;
  gid_t gid = 0;
  struct timespec atime, mtime, ctime;

  // Read the header, and return NULL if there can't be a header.
  if (tarfs_read(&header, sizeof(header), offset, sb) != sizeof(header)) {
    pr_err("tarfs: read failure");
    return NULL;
  }

  // Check for the header magic value
  if (memcmp(header.magic, OLDGNU_MAGIC, sizeof(header.magic)) != 0) {
    return NULL;
  }

  // Parse the data length from the header
  if (kstrtouint(header.size, OCTAL, &length) != 0) {
    pr_info("tarfs: failed to read size");
    return NULL;
  }

  // Parse mode
  if (kstrtouint(header.mode, OCTAL, &mode) != 0) {
    pr_info("tarfs: failed to read mode");
    return NULL;
  }

  if (kstrtouint(header.uid, OCTAL, &uid) != 0) {
    pr_info("tarfs: failed to read uid");
    return NULL;
  }

  if (kstrtouint(header.gid, OCTAL, &gid) != 0) {
    pr_info("tarfs: failed to read gid");
    return NULL;
  }

  // Modification time is the most likely to be present
  if (kstrtoul(header.mtime, OCTAL, &mtime.tv_sec) != 0) {
    mtime.tv_sec = 0;
    mtime.tv_nsec = 0;
  }

  if (kstrtoul(header.atime, OCTAL, &atime.tv_sec) != 0) {
    atime = mtime; // Copy mtime if not set
  }

  if (kstrtoul(header.ctime, OCTAL, &ctime.tv_sec) != 0) {
    ctime = mtime; // Copy mtime if not set
  }

  // Build the full name of the entry
  full_name = build_name(&header);
  if (!full_name) {
    pr_info("tarfs: name allocation error");
    return NULL;
  }

  // Split path name into dirname and basename.  If the file resides in the
  // root, take the full name as basename and point dirname to the trailing
  // NUL byte.  Else, NUL the last slash ("/") and set the pointers accordingly.
  basename = strrchr(full_name, '/');
  if (basename) {
    *basename = 0x0;
    basename++;
  } else {
    basename = full_name;
    full_name = basename + strlen(basename);
  }

  // Fill in structure
  entry = kzalloc(sizeof(struct tar_entry), GFP_KERNEL);
  entry->header = header;
  entry->dirname = full_name;
  entry->basename = basename;
  entry->offset = offset;
  entry->data_offset = offset + sizeof(header) + ALIGN_SECTOR(sizeof(header));
  entry->length = length;
  entry->mode = mode;
  entry->uid = uid;
  entry->gid = gid;
  entry->atime = atime;
  entry->mtime = mtime;
  entry->ctime = ctime;
  return entry;
}

struct tar_entry *tar_open(struct super_block *sb)
{
  struct tar_entry *first = tar_read_entry(sb, 0);
  struct tar_entry *parent = first;
  struct tar_entry *next;
  unsigned long count = 2;
  off_t offset = 0;
  off_t length;

  while (parent) {
    parent->inode = count; // Assign inode number
    count++;

    length = parent->data_offset + parent->length;

    // Skip leading data of the previously read entry
    offset = length + ALIGN_SECTOR(length);

    // Read next entry
    next = tar_read_entry(sb, offset);
    parent->next = next;
    parent = next;
  }

  return first;
}

void tar_free(struct tar_entry *entry)
{
  while (entry) {
    struct tar_entry *next = entry->next;

    if (entry->dirname < entry->basename)
      kfree(entry->dirname);
    else
      kfree(entry->basename);

    kfree(entry);
    entry = next;
  }
}

size_t tar_read(struct super_block *sb, struct tar_entry *entry,
                unsigned int off, void *buffer, size_t len)
{
  size_t to_read = min_t(size_t, len, entry->length - off);
  return tarfs_read(buffer, to_read, entry->data_offset + off, sb);
}

struct tar_entry *tar_find(struct tar_entry *entry, const char *dirname,
                           const char *basename)
{
  while (entry) {
    if (!strcmp(entry->basename, basename) &&
        !strcmp(entry->dirname, dirname)) {
      return entry;
    }

    entry = entry->next;
  }

  return NULL;
}
