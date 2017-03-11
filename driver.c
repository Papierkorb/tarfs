#include <linux/init.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>

#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mutex.h>

#include "tar.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Stefan Merettig");
MODULE_DESCRIPTION("A tar filesystem driver");
MODULE_VERSION("0.1");

#define WRITE_MASK (0222)
#define ROOT_INO_MODE (S_IFDIR | 0555)
#define ROOT_INO (0)

static struct super_operations tarfs_super_ops;
static const struct inode_operations tarfs_dir_inode_operations;
static const struct inode_operations tarfs_file_inode_operations;
static const struct inode_operations tarfs_symlink_inode_operations;
static const struct file_operations tarfs_dir_operations;
static const struct file_operations tarfs_file_operations;

/**
 * @brief Finds an tar entry by its inode number.
 * @param entry the start entry
 * @param inode the inode number
 * @return the found entry or \c NULL
 */
static struct tar_entry *tarfs_find_by_inode(struct tar_entry *entry,
                                             unsigned long inode)
{
  while (entry) {
    if (entry->inode == inode)
      return entry;
    entry = entry->next;
  }

  return NULL;
}

/**
 * @brief Reads from a file.
 * @param file the accessed file
 * @param userbuf the target buffer in \b user-space
 * @param count byte count to read
 * @param pos the position to read from, updated afterwards
 * @return count of bytes read
 */
static ssize_t tarfs_file_read(struct file *file, char __user *userbuf,
                                   size_t count, loff_t *pos)
{
  struct inode *inode = file_inode(file);
  struct super_block *sb = inode->i_sb;
  struct tar_entry *root = (struct tar_entry *) sb->s_fs_info;
  struct tar_entry *entry;
  size_t advanced;
  unsigned long not_copied;
  void *buffer = NULL;

  if (!root)
    return -ENOENT;

  entry = tarfs_find_by_inode(root, inode->i_ino);
  if (!entry)
    return -ENOENT;

  buffer = kzalloc(count, GFP_KERNEL);
  advanced = tar_read(sb, entry, *pos, buffer, count);
  not_copied = copy_to_user(userbuf, buffer, advanced);
  kfree(buffer);

  if (not_copied > advanced)
    return -EBADF; // Is this a good error code?

  advanced -= not_copied;
  *pos += advanced;

  return advanced;
}

/**
 * @brief Releases a file handle.
 * @param inode the affected inode
 * @param file the file to close
 * @return \c 0
 */
static int tarfs_file_release(struct inode *inode, struct file *file)
{
  return 0;
}

/**
 * @brief Returns the POSIX ACL of a inode.  Not supported, thus always \c NULL
 * @param inode the inode
 * @param flags lookup flags
 * @return \c NULL
 */
static struct posix_acl *tarfs_get_acl(struct inode *inode, int flags)
{
  return NULL;
}

/**
 * @brief Returns the POSIX file mode of \a entry.
 * @param entry the entry to get the file mode from
 * @return the file mode
 */
static mode_t tarfs_entry_mode(struct tar_entry *entry)
{
  mode_t mode = entry->mode & ~WRITE_MASK;
  mode |= tar_type_to_posix(entry->header.typeflag);
  return mode;
}

/**
 * @brief Returns the full path to an \a entry.
 * @param entry the entry to get the path from
 * @return Allocated string of the path, must be freed later
 */
static char *tarfs_full_name(struct tar_entry *entry)
{
  size_t dirlen = strlen(entry->dirname);
  size_t baselen = strlen(entry->basename);
  size_t len = dirlen + baselen + 2;
  char *result = kzalloc(len, GFP_KERNEL);

  if (!result)
    return NULL;

  // Don't prepend a "/" in for top-level entries.
  if (dirlen < 1)
    strncpy(result, entry->basename, baselen);
  else
    snprintf(result, len, "%s/%s", entry->dirname, entry->basename);

  return result;
}

/**
 * @brief Builds an inode structure out of an \a entry.
 * @param sb the super block
 * @param entry the entry
 * @return a filled inode structure
 */
static struct inode *tarfs_build_inode(struct super_block *sb,
                                       struct tar_entry *entry)
{
  struct inode *inode = new_inode(sb);
  mode_t mode = tarfs_entry_mode(entry);

  if (!inode)
    return NULL;

  inode->i_ino = entry->inode;
  inode->i_mode = mode;
  inode->i_uid.val = entry->uid;
  inode->i_gid.val = entry->gid;
  inode->i_size = entry->length;
  inode->i_atime = entry->atime;
  inode->i_mtime = entry->mtime;
  inode->i_ctime = entry->ctime;

  switch (entry->header.typeflag) {
    case DIRTYPE:
      inode->i_op = &tarfs_dir_inode_operations;
      inode->i_fop = &tarfs_dir_operations;
      break;
    case SYMTYPE:
      inode->i_link = entry->header.linkname;
      inode->i_op = &tarfs_symlink_inode_operations;
      break;
    default:
      inode->i_op = &tarfs_file_inode_operations;
      inode->i_fop = &tarfs_file_operations;
      break;
  }

  return inode;
}

/**
 * @brief Builds the lookup path for the \a dir.
 * @param dir the directory to get a path to
 * @param entry the root entry
 * @return the path, must be freed later on
 */
static char *build_lookup_path(struct inode *dir, struct tar_entry *entry)
{
  if (dir->i_ino == ROOT_INO)
    return kzalloc(1, GFP_KERNEL);

  entry = tarfs_find_by_inode(entry, dir->i_ino);
  if (!entry) // Unknown directory?
    return NULL;

  return tarfs_full_name(entry);
}

/**
 * @brief Looks up an entry by an inode for user access
 * @param dir the containing directory
 * @param dentry the entry to look up
 * @param flags lookup flags
 * @return the found directory entry
 */
static struct dentry *tarfs_lookup(struct inode *dir, struct dentry *dentry,
                                   unsigned int flags)
{
  struct tar_entry *entry;
  struct tar_entry *found_entry;
  char *dir_path = NULL;
  struct inode *inode = NULL;

  if (!dir->i_sb)
    pr_err("Missing superblock in inode %lu", dir->i_ino);

  entry = (struct tar_entry *) dir->i_sb->s_fs_info;
  dir_path = build_lookup_path(dir, entry);

  // Search for entry in the directory
  found_entry = tar_find(entry, dir_path, dentry->d_name.name);
  if (found_entry)
    inode = tarfs_build_inode(dir->i_sb, found_entry);

  kfree(dir_path);
	return d_splice_alias(inode, dentry);
}

/**
 * @brief Iterates over the directory in \a file, emitting found entries.
 * @param file the directory to iterate on
 * @param ctx the iteration context to emit to
 * @return an error code
 */
static int tarfs_readdir(struct file *file, struct dir_context *ctx)
{
  struct inode *inode = file_inode(file);
  struct super_block *sb = inode->i_sb;
  struct tar_entry *entry = (struct tar_entry *) sb->s_fs_info;
  char *dir_path = build_lookup_path(inode, entry);
  int namelen;

  if (ctx->pos > 0)
    goto out;

  // Tell the reader the total count of elements.
  if (!dir_emit_dots(file, ctx))
    goto out;

  while (entry) {
    if (!strcmp(dir_path, entry->dirname)) {
      ctx->pos++;

      namelen = strlen(entry->basename);
      if (!dir_emit(ctx, entry->basename, namelen, entry->inode, entry->mode >> 12))
        break;
    }

    entry = entry->next;
  }

out:
  kfree(dir_path);
  return 0;
}

/**
 * @brief Sets up the \a sb.
 * @param sb the super block
 * @param data
 * @param silent if messages shall be suppressed
 * @return an error code
 */
static int tarfs_fill_sb(struct super_block *sb, void *data, int silent)
{
  struct inode *root = NULL;
  struct tar_entry *entry = NULL;

  sb->s_flags |= MS_RDONLY | MS_NOATIME; /* This fs is read-only */
  sb->s_op = &tarfs_super_ops;

  if (!(entry = tar_open(sb))) {
    pr_err("failed to read tar index");
    return -ENOMEM;
  }

  sb->s_fs_info = entry;

  if (!(root = new_inode(sb))) {
    pr_err("failed to allocate root inode");
    return -ENOMEM;
  }

  root->i_ino = ROOT_INO;
  root->i_sb = sb;
  root->i_op = &tarfs_dir_inode_operations;
  root->i_fop = &tarfs_dir_operations;
  root->i_atime = CURRENT_TIME;
  root->i_mtime = CURRENT_TIME;
  root->i_ctime = CURRENT_TIME;
  inode_init_owner(root, NULL, ROOT_INO_MODE);

  if (!(sb->s_root = d_make_root(root))) {
    pr_err("failed to create root inode");
    return -ENOMEM;
  }

  return 0;
}

/**
 * @brief Called by linux to mount \a dev.
 * @param type our file system type
 * @param flags mount flags
 * @param dev the device path to mount
 * @param data
 * @return the root directory entry
 */
static struct dentry *tarfs_mount(struct file_system_type *type, int flags,
                                  char const *dev, void *data)
{
  return mount_bdev(type, flags, dev, data, tarfs_fill_sb);
}

/**
 * @brief Called by linux to unmount \a sb.
 * @param sb the super block of the instance to unmount
 */
static void tarfs_kill_sb(struct super_block *sb)
{
  struct tar_entry *entry = (struct tar_entry *) sb->s_fs_info;

  tar_free(entry);
  kill_litter_super(sb);
}

static struct super_operations tarfs_super_ops = {
};

static const struct file_operations tarfs_file_operations = {
  .llseek   = generic_file_llseek,
  .read     = tarfs_file_read,
  .open     = generic_file_open,
  .release  = tarfs_file_release,
};

static const struct inode_operations tarfs_file_inode_operations = {
  .get_acl  = tarfs_get_acl,
};

static const struct file_operations tarfs_dir_operations = {
  .llseek		       = generic_file_llseek,
  .read		         = generic_read_dir,
  .iterate_shared  = tarfs_readdir,
};

static const struct inode_operations tarfs_dir_inode_operations = {
	.lookup	  = tarfs_lookup,
  .get_acl  = tarfs_get_acl,
};

static const struct inode_operations tarfs_symlink_inode_operations = {
	.get_link	 = simple_get_link,
  .readlink  = generic_readlink,
  .get_acl   = tarfs_get_acl,
};

static struct file_system_type tarfs_type = {
  .owner     = THIS_MODULE,
  .name      = "tarfs",
  .mount     = tarfs_mount,
  .kill_sb   = tarfs_kill_sb,
  .fs_flags  = FS_REQUIRES_DEV,
};

/** @brief Called by linux to initialize the module. */
static int __init tarfs_init(void)
{
   pr_info("tarfs: Initializing");
   return register_filesystem(&tarfs_type);
}

/** @brief Called by linux to unload the module. */
static void __exit tarfs_exit(void)
{
   pr_info("tarfs: Exiting");
   unregister_filesystem(&tarfs_type);
}

module_init(tarfs_init);
module_exit(tarfs_exit);
