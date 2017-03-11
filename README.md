# tarfs

tarfs is implemented as Linux kernel module filesystem driver.  It's more of a
research thing.  If you're interested in actually using something like this,
consider writing it using libFuse :)

## Features

* Supports GNU tar files
* Regular files, directories and symlinks
* UID, GID, access/modification/creation time
* Read-only access to files and directories

## Compiling

You'll require the linux development files.  For ArchLinux, you need the
`linux-headers` packet.

After that, just run `make` to build the `tarfs.ko`, which is a loadable
kernel module.

**Note**: The module was tested with Linux `4.9.11`, on a `x64 ArchLinux`
          computer.

## Usage

```sh
# If not already done, build the module
make

# Now you can load the module:
sudo insmod tarfs.ko

# You need a mount directory
mkdir mnt

# Mount some tar archive.  A sample one is included:
sudo mount sample.tar -o loop -t tarfs mnt

# Discover the archives content
ls mnt -R
cat mnt/hello.c

# Unmount
sudo umount mnt

# And unload the kernel module
sudo rmmod tarfs.ko
```

## File overview

* **driver.c** The driver code interfacing with Linux
* **device.c/h** Code to read from the underlying block device
* **tar.c/h** Code to read the tar file
* **gnutar.h** Header definition for tar files, taken from
  https://www.gnu.org/software/tar/manual/html_node/Standard.html

## Attention

Please note that you're actually loading stuff into your kernel.  That means the
module runs with highest permissions possible (Ring 0 on x86 machines).  Also,
if something goes really wrong, your entire computer could hang/crash/freeze.

It's recommended to try this in a virtual machine.  You can create one easily
using programs like `QEMU` or `VirtualBox`.

## License

The enclosed source code, and the `sample.tar` file including its contents, are
subject to the **General Public License version 3** (**GPLv3**).  Please see the
included `LICENSE` file for the whole license text.  If you're interested in a
legally non-binding explanation of this license, have a look at
[its tl;drLegal page](https://tldrlegal.com/license/gnu-general-public-license-v3-(gpl-3)).

## Still reading?

Thanks for your interest - Have a nice day, and happy hacking!
