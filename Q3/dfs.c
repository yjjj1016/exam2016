/*
 * a .....  Filesystem
 Eric McCreath 2016 - GPL

 (based on the simplistic RAM filesystem McCreath 2001 and
 the very very simple filesystem 2006,2008,2010,2016 )   */

/* to make just use:
 make

 to load use:
 sudo insmod dfs.ko
 (may need to copy dfs.ko to a local filesystem first)

 to make a suitable filesystem:
 dd of=dfs.raw if=/dev/zero bs=512 count=100
 ./mkfs.dfs dfs.raw
 (could also use a USB device etc.)

 to mount use:
 mkdir testdir
 sudo mount -o loop -t dfs dfs.raw testdir

 use the file system:
 cd testdir
 echo hello > file1
 cat file1
 cd ..

 unmount the filesystem:
 sudo umount testdir

 remove the module:
 sudo rmmod dfs
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/statfs.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <asm/uaccess.h>

#include "dfs.h"

#define DEBUG 1

static struct inode_operations dfs_file_inode_operations;
static struct file_operations dfs_file_operations;
static struct super_operations dfs_ops;

struct inode *dfs_iget(struct super_block *sb, unsigned long ino);

// dfs_put_super - no super block on disk so do nothing.
static void dfs_put_super(struct super_block *sb) {
	if (DEBUG)
		printk("dfs - put_super\n");
	return;
}


// dfs_statfs - get info about fs
static int dfs_statfs(struct dentry *dentry, struct kstatfs *buf) {
	if (DEBUG)
		printk("dfs - statfs\n");

	buf->f_namelen = MAXNAME;
	return 0;
}

// dfs_readblock - reads a block from the block device (this will copy over
//                      the top of inode)
static int dfs_readblock(struct super_block *sb, int inum,
		struct dfs_inode *inode) {
	struct buffer_head *bh;

	if (DEBUG)
		printk("dfs - readblock : %d\n", inum);

	bh = sb_bread(sb, inum);
	memcpy((void *) inode, (void *) bh->b_data, BLOCKSIZE);
	brelse(bh);
	if (DEBUG)
		printk("dfs - readblock done : %d\n", inum);
	return BLOCKSIZE;
}

// dfs_writeblock - write a block from the block device(this will just mark the block
//                      as dirtycopy)
static int dfs_writeblock(struct super_block *sb, int inum,
		struct dfs_inode *inode) {
	struct buffer_head *bh;

	if (DEBUG)
		printk("dfs - writeblock : %d\n", inum);

	bh = sb_bread(sb, inum);
	memcpy(bh->b_data, inode, BLOCKSIZE);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	if (DEBUG)
		printk("dfs - writeblock done: %d\n", inum);
	return BLOCKSIZE;
}

// dfs_readdir - reads a directory and places the result using filldir
static int
dfs_readdir(struct file *filp, struct dir_context *ctx) {
	struct inode *i;
	struct dfs_inode dirdata;
	int num_dirs;
	struct dfs_dir_entry dent;
	char *dentstart;
	int error, k, j;

	if (DEBUG)
		printk("dfs - readdir\n");

	i = file_inode(filp);

	dfs_readblock(i->i_sb, i->i_ino, &dirdata);

	num_dirs = dirdata.size / sizeof(struct dfs_dir_entry);

	if (DEBUG)
		printk("Number of entries %d fpos %Ld\n", num_dirs, filp->f_pos);

	error = 0;
	k = 0;

	dentstart = (char *) &dent;
	while (!error && (filp->f_pos < (num_dirs * sizeof(struct dfs_dir_entry)))
			&& k < num_dirs) {

		for (j = 0; j < sizeof(struct dfs_dir_entry); j++) {
			dfs_readblock(i->i_sb, k * sizeof(struct dfs_dir_entry) + j,
					&dirdata);
			memcpy(dentstart + j, dirdata.data + i->i_ino, 1);
		}
		printk("adding name : %s ino : %d\n", dent.name, dent.inode_number);

		if (dent.inode_number) {
			if (!dir_emit(ctx, dent.name, strnlen(dent.name, MAXNAME),
					dent.inode_number, DT_UNKNOWN))
				return 0;
		}
		ctx->pos += sizeof(struct dfs_dir_entry);
		k++;
	}
	printk("done readdir\n");

	return 0;
}

// dfs_lookup - A directory name in a directory. It basically attaches the inode 
//                of the file to the directory entry.
static struct dentry *
dfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {

	int num_dirs;
	int k, i;
	struct dfs_inode dirdata;
	struct inode *inode = NULL;
	struct dfs_dir_entry dent;
	char *dentstart;

	if (DEBUG)
		printk("dfs - lookup\n");

	dfs_readblock(dir->i_sb, dir->i_ino, &dirdata);
	num_dirs = dirdata.size / sizeof(struct dfs_dir_entry);

	for (k = 0; k < num_dirs; k++) {
		dentstart = (char *) &dent;
		for (i = 0; i < sizeof(struct dfs_dir_entry); i++) {
			dfs_readblock(dir->i_sb,
					k * sizeof(struct dfs_dir_entry) + i, &dirdata);
			memcpy(dentstart + i, dirdata.data + dir->i_ino, 1);
		}

		if ((strlen(dent.name) == dentry->d_name.len)
				&& strncmp(dent.name, dentry->d_name.name, dentry->d_name.len)
						== 0) {
			inode = dfs_iget(dir->i_sb, dent.inode_number);

			if (!inode)
				return ERR_PTR(-EACCES);

			d_add(dentry, inode);
			return NULL;

		}
	}
	d_add(dentry, inode);
	return NULL;
}

// dfs_empty_inode - finds the first free inode (returns -1 is unable to find one)
static int dfs_empty_inode(struct super_block *sb) {
	struct dfs_inode block;
	int k;
	for (k = 0; k < NUMBLOCKS; k++) {
		dfs_readblock(sb, k, &block);
		if (block.is_empty)
			return k;
	}
	return -1;
}

// dfs_new_inode - find and construct a new inode.
struct inode * dfs_new_inode(const struct inode * dir, umode_t mode) {
	struct dfs_inode block;
	struct super_block * sb;
	struct inode * inode;
	int newinodenumber;

	if (DEBUG)
		printk("dfs - new inode\n");

	if (!dir)
		return NULL;
	sb = dir->i_sb;

	/* get an vfs inode */
	inode = new_inode(sb);
	if (!inode)
		return NULL;

	/* find a spare inode in the dfs */
	newinodenumber = dfs_empty_inode(sb);
	if (newinodenumber == -1) {
		printk("dfs - inode table is full.\n");
		return NULL;
	}
	dfs_readblock(sb, newinodenumber, &block);
	block.is_empty = false;
	block.size = 0;
	block.is_directory = false;
	dfs_writeblock(sb, newinodenumber, &block);

	inode_init_owner(inode, dir, mode);
	inode->i_ino = newinodenumber;
	inode->i_ctime = inode->i_mtime = inode->i_atime = CURRENT_TIME;

	inode->i_op = NULL;

	insert_inode_hash(inode);

	return inode;
}

// dfs_create - create a new file in a directory 
static int dfs_create(struct inode *dir, struct dentry* dentry, umode_t mode,
		bool excl) {
	int i;
	struct dfs_inode dirdata;
	int num_dirs;
	struct dfs_dir_entry dent;
	char *dentstart;

	struct inode * inode;

	if (DEBUG)
		printk("dfs - create : %s\n", dentry->d_name.name);

	inode = dfs_new_inode(dir, S_IRUGO | S_IWUGO | S_IFREG);
	if (!inode)
		return -ENOSPC;
	inode->i_op = &dfs_file_inode_operations;
	inode->i_fop = &dfs_file_operations;
	inode->i_mode = mode;

	/* get an vfs inode */
	if (!dir)
		return -1;

	dfs_readblock(dir->i_sb, dir->i_ino, &dirdata);
	num_dirs = dirdata.size / sizeof(struct dfs_dir_entry);

	dirdata.size = (num_dirs + 1) * sizeof(struct dfs_dir_entry);

	dfs_writeblock(dir->i_sb, dir->i_ino, &dirdata);

	strncpy(dent.name, dentry->d_name.name, dentry->d_name.len);
	dent.name[dentry->d_name.len] = '\0';
	dent.inode_number = inode->i_ino;

	dentstart = (char *) &dent;
	for (i = 0; i < sizeof(struct dfs_dir_entry); i++) {
		dfs_readblock(dir->i_sb, num_dirs * sizeof(struct dfs_dir_entry) + i,
				&dirdata);
		memcpy(dirdata.data + dir->i_ino, dentstart + i, 1);
		dfs_writeblock(dir->i_sb, num_dirs * sizeof(struct dfs_dir_entry) + i,
				&dirdata);
	}

	d_instantiate(dentry, inode);

	printk("File created %ld\n", inode->i_ino);
	return 0;
}

// dfs_file_write - write to a file
static ssize_t dfs_file_write(struct file *filp, const char *buf, size_t count,
		loff_t *ppos) {
	int i;
	struct dfs_inode filedata;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,19,0)
	struct inode *inode = filp->f_dentry->d_inode;
#else
	struct inode *inode = filp->f_path.dentry->d_inode;
#endif
	ssize_t pos;
	struct super_block * sb;
	char * p;

	if (DEBUG)
		printk("dfs - file write - count : %zu ppos %Ld\n", count, *ppos);

	if (!inode) {
		printk("dfs - Problem with file inode\n");
		return -EINVAL;
	}

	if (!(S_ISREG(inode->i_mode))) {
		printk("dfs - not regular file\n");
		return -EINVAL;
	}
	if (*ppos > inode->i_size || count <= 0) {
		printk("dfs - attempting to write over the end of a file.\n");
		return 0;
	}
	sb = inode->i_sb;

	if (filp->f_flags & O_APPEND)
		pos = inode->i_size;
	else
		pos = *ppos;

	if (pos + count > NUMBLOCKS)
		return -ENOSPC;

       // *******************************************************
       // Add your answers as a comment to part 1 of question 3 here
       // Part 1. 
       /*





       */
       // About 12 lines of code have been removed from here.  
       // Add the code that completes the dfs_file_write function
       // recovering this removed code (or code that is functionally 
       // identical). This is the only part of the code that you need 
       // to, or should, change.
       // Part 2.



       // *******************************************************


	*ppos += count;

	if (DEBUG)
		printk("dfs - file write done : %zu ppos %Ld\n", count, *ppos);

	return count;
}

// dfs_file_read - read data from a file
static ssize_t dfs_file_read(struct file *filp, char *buf, size_t count,
		loff_t *ppos) {
	int i;
	struct dfs_inode filedata;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,19,0)
	struct inode *inode = filp->f_dentry->d_inode;
#else
	struct inode *inode = filp->f_path.dentry->d_inode;
#endif
	ssize_t offset, size;

	struct super_block * sb;

	if (DEBUG)
		printk("dfs - file read - count : %zu ppos %Ld\n", count, *ppos);

	if (!inode) {
		printk("dfs - Problem with file inode\n");
		return -EINVAL;
	}

	if (!(S_ISREG(inode->i_mode))) {
		printk("dfs - not regular file\n");
		return -EINVAL;
	}
	if (*ppos > inode->i_size || count <= 0) {
		printk("dfs - attempting to read over the end of a file.\n");
		return 0;
	}
	sb = inode->i_sb;

	size = MIN(inode->i_size - *ppos, count);

	printk("dfs - read data\n");

	offset = *ppos;

	for (i = 0; i < size; i++) {
		dfs_readblock(sb, offset + i, &filedata);
		if (copy_to_user(buf + i, filedata.data + inode->i_ino, 1))
			return -EIO;
	}

	*ppos += size;
	return size;
}

static struct file_operations dfs_file_operations = { 
	read: dfs_file_read, 
	write: dfs_file_write, 
};

static struct inode_operations dfs_file_inode_operations = { };

static struct file_operations dfs_dir_operations = { 
	.llseek = generic_file_llseek, 
	.read = generic_read_dir, 
	.iterate = dfs_readdir,
	.fsync = generic_file_fsync, };

static struct inode_operations dfs_dir_inode_operations = { 
	create: dfs_create, 
	lookup: dfs_lookup,
};

// dfs_iget - get the inode from the super block
struct inode *dfs_iget(struct super_block *sb, unsigned long ino) {
	struct inode *inode;
	struct dfs_inode filedata;

	if (DEBUG) {
		printk("dfs - iget - ino : %d", (unsigned int) ino);
		printk(" super %p\n", sb);
	}

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	dfs_readblock(inode->i_sb, inode->i_ino, &filedata);

	inode->i_size = filedata.size;

	inode->i_ctime = inode->i_mtime = inode->i_atime = CURRENT_TIME;

	if (filedata.is_directory) {
		inode->i_mode = S_IRUGO | S_IWUGO | S_IFDIR;
		inode->i_op = &dfs_dir_inode_operations;
		inode->i_fop = &dfs_dir_operations;
	} else {
		inode->i_mode = S_IRUGO | S_IWUGO | S_IFREG;
		inode->i_op = &dfs_file_inode_operations;
		inode->i_fop = &dfs_file_operations;
	}

	unlock_new_inode(inode);
	return inode;
}

// dfs_fill_super - read the super block (this is simple as we do not
//                    have one in this file system)
static int dfs_fill_super(struct super_block *s, void *data, int silent) {
	struct inode *i;
	int hblock;

	if (DEBUG)
		printk("dfs - fill super\n");

	s->s_flags = MS_NOSUID | MS_NOEXEC;
	s->s_op = &dfs_ops;

	i = new_inode(s);

	i->i_sb = s;
	i->i_ino = 0;
	i->i_flags = 0;
	i->i_mode = S_IRUGO | S_IWUGO | S_IXUGO | S_IFDIR;
	i->i_op = &dfs_dir_inode_operations;
	i->i_fop = &dfs_dir_operations;
	printk("inode %p\n", i);

	hblock = bdev_logical_block_size(s->s_bdev);
	if (hblock > BLOCKSIZE) {
		printk("device blocks are too small!!");
		return -1;
	}

	set_blocksize(s->s_bdev, BLOCKSIZE);
	s->s_blocksize = BLOCKSIZE;
	s->s_blocksize_bits = BLOCKSIZE_BITS;
	s->s_root = d_make_root(i);

	return 0;
}

static struct super_operations dfs_ops = { 
	statfs: dfs_statfs, 
	put_super: dfs_put_super, };

// dfs_mount - mount the file system
static struct dentry *dfs_mount(struct file_system_type *fs_type, int flags,
		const char *dev_name, void *data) {
	return mount_bdev(fs_type, flags, dev_name, data, dfs_fill_super);
}

static struct file_system_type dfs_type = { 
	.owner = THIS_MODULE, .name = "dfs",
	.mount = dfs_mount,
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV, };

// dfs_init - register the file system
static int __init dfs_init(void)
{
	printk("Registering dfs\n");
	return register_filesystem(&dfs_type);
}

// dfs_exit - ungegister the file system
static void __exit dfs_exit(void)
{
	printk("Unregistering the dfs.\n");
	unregister_filesystem(&dfs_type);
}

module_init( dfs_init);
module_exit( dfs_exit);
MODULE_LICENSE("GPL");
