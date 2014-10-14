/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#define _XOPEN_SOURCE 600
#define _GNU_SOURCE 1

#include "kerncompat.h"

#include <sys/ioctl.h>
#include <sys/mount.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/acl.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include <uuid/uuid.h>
 #include <errno.h>

#include "ctree.h"
#include "disk-io.h"
#include "volumes.h"
#include "transaction.h"
#include "crc32c.h"
#include "utils.h"
#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2fs.h>
#include <ext2fs/ext2_ext_attr.h>

/*
* Record a file extent. Do all the required works, such as inserting
* file extent item, inserting extent item and backref item into extent
* tree and updating block accounting.
*/
int btrfs_record_composed_file_extent(struct btrfs_trans_handle *trans,
	struct btrfs_root *root, u64 objectid, 
	struct btrfs_inode_item *inode,
	u64 file_pos, u64 disk_bytenr,
	u64 num_bytes)
{
	int ret;
	struct btrfs_fs_info *info = root->fs_info;
	struct btrfs_root *extent_root = info->extent_root;
	struct extent_buffer *leaf;
	struct btrfs_file_extent_item *fi;
	struct btrfs_key ins_key;
	struct btrfs_path path;
	struct btrfs_extent_item *ei;
	u64 nbytes, size;

	if (disk_bytenr == 0) {
	ret = btrfs_insert_file_extent(trans, root, objectid,
		file_pos, disk_bytenr,
		num_bytes, num_bytes);
	return ret;
	}

	btrfs_init_path(&path);

	ins_key.objectid = objectid;
	ins_key.offset = file_pos;
	btrfs_set_key_type(&ins_key, BTRFS_EXTENT_DATA_KEY);
	ret = btrfs_insert_empty_item(trans, root, &path, &ins_key,
		sizeof(*fi));
	if (ret)
		goto fail;
	leaf = path.nodes[0];
	fi = btrfs_item_ptr(leaf, path.slots[0],
		struct btrfs_file_extent_item);

	btrfs_set_file_extent_generation(leaf, fi, trans->transid);
	btrfs_set_file_extent_type(leaf, fi, BTRFS_FILE_EXTENT_REG);
	btrfs_set_file_extent_disk_bytenr(leaf, fi, disk_bytenr);
	btrfs_set_file_extent_disk_num_bytes(leaf, fi, num_bytes);
	btrfs_set_file_extent_offset(leaf, fi, 0);
	btrfs_set_file_extent_num_bytes(leaf, fi, num_bytes);
	btrfs_set_file_extent_ram_bytes(leaf, fi, num_bytes);
	btrfs_set_file_extent_compression(leaf, fi, 0);
	btrfs_set_file_extent_encryption(leaf, fi, 0);
	btrfs_set_file_extent_other_encoding(leaf, fi, 0);
	btrfs_mark_buffer_dirty(leaf);

	nbytes = btrfs_inode_nbytes(leaf, inode) + num_bytes;
	size = btrfs_inode_size(leaf, inode) + num_bytes;
	btrfs_set_inode_nbytes(leaf, inode, nbytes);
	btrfs_set_inode_size(leaf, inode, size);

	btrfs_release_path(&path);

	ins_key.objectid = disk_bytenr;
	ins_key.offset = num_bytes;
	ins_key.type = BTRFS_EXTENT_ITEM_KEY;

	ret = btrfs_insert_empty_item(trans, extent_root, &path,
		&ins_key, sizeof(*ei));

	if (ret == 0) {
		leaf = path.nodes[0];
		ei = btrfs_item_ptr(leaf, path.slots[0],
		struct btrfs_extent_item);

		btrfs_set_extent_refs(leaf, ei, 0);
		btrfs_set_extent_generation(leaf, ei, 0);
		btrfs_set_extent_flags(leaf, ei, BTRFS_EXTENT_FLAG_DATA);

		btrfs_mark_buffer_dirty(leaf);

		ret = btrfs_update_block_group(trans, root, disk_bytenr,
		       num_bytes, 1, 0);
		if (ret)
		goto fail;
	} else if (ret != -EEXIST) {
		goto fail;
	}
	btrfs_extent_post_op(trans, extent_root);

	ret = btrfs_inc_extent_ref(trans, root, disk_bytenr, num_bytes, 0,
		root->root_key.objectid,
		objectid, file_pos);
	
	if (ret)
		goto fail;
	ret = 0;

fail:
	btrfs_release_path(&path);
	return ret;
}

static int do_compose(const char *devname, const char *filename, 
	const char *tgtdevice, const unsigned long long tgtoffset,
	const unsigned long long num_bytes,
	int datacsum, int noxattr)
{
	
	fprintf(stdout, "Composing file %s\n", filename);

	struct btrfs_inode_item *inode;
	int devfd;
	int ret = 0;
	struct btrfs_path path;
	struct btrfs_dir_item *dir;
	struct btrfs_root *root;	
	u64 root_dir, total_bytes, size, objectid;
	struct extent_buffer *leaf;
	struct btrfs_trans_handle *trans;
	struct btrfs_key key;

	devfd = open(devname, O_RDWR | O_SYNC);
	if (devfd < 0) {
		fprintf(stderr, "unable to open %s\n", devname);
		goto fail;
	}
	
	root = open_ctree_fd(devfd, devname, 0, OPEN_CTREE_WRITES);
	
	btrfs_init_path(&path);
	root_dir = btrfs_root_dirid(&root->fs_info->fs_root->root_item);

	if (root != NULL) {
		fprintf(stdout, "fs ID is %u, last alloc inode %llu\n", root->fs_info->fsid[0], 
			root->fs_info->fs_root->last_inode_alloc);
	}
	dir = btrfs_lookup_dir_item(NULL, root, &path,
		root_dir, filename, strlen(filename), 0);
	
	if (dir == NULL || IS_ERR(dir)) {
		fprintf(stderr, "unable to find file %s\n", filename);
		goto fail;
	}

	leaf = path.nodes[0];
	btrfs_dir_item_key_to_cpu(leaf, dir, &key);
	btrfs_release_path(&path);
	objectid = key.objectid;
	fprintf(stdout, "found dir key %llu\n", key.objectid);

	ret = btrfs_lookup_inode(NULL, root, &path, &key, 0);
	fprintf(stdout, "found inode key %llu\n", key.objectid);

	if (ret) {
		fprintf(stderr, "unable to find inode item\n");
		goto fail;
	}

	leaf = path.nodes[0];
	inode = btrfs_item_ptr(leaf, path.slots[0], struct btrfs_inode_item);
	total_bytes = btrfs_inode_nbytes(leaf, inode);
	// total_bytes = btrfs_stack_inode_nbytes(inode);
	size = btrfs_inode_size(leaf, inode);
	fprintf(stdout, "old total bytes %llu, size is %llu\n", total_bytes, size);
	btrfs_release_path(&path);

	trans = btrfs_start_transaction(root, 1);
	if (!trans) {
		return -ENOMEM;
	}
	fprintf(stdout, "objectid is %llu\n", key.objectid);
	ret = btrfs_record_composed_file_extent(trans, root, objectid, inode, total_bytes,
						tgtoffset, num_bytes);
	if (ret) {
		fprintf(stderr, "btrfs_record_composed_file_extent returned %d\n", ret);
	}
	// btrfs_set_inode_nbytes(leaf, inode, total_bytes + 1048576);
	// btrfs_set_inode_size(leaf, inode, size + 1048576);
	total_bytes = btrfs_inode_nbytes(leaf, inode);
	size = btrfs_inode_size(leaf, inode);
	fprintf(stdout, "new total size %llu, size is %llu\n", total_bytes, size);
	// btrfs_insert_inode(trans, root, key.objectid, inode);
	btrfs_mark_buffer_dirty(leaf);
	btrfs_release_path(&path);
	// btrfs_free_path(&path);
	ret = btrfs_commit_transaction(trans, root);
	if (ret) {
		fprintf(stderr, "btrfs_commit_transaction returned %d\n", ret);
	}
	ret = close_ctree(root);
	if (ret) {
		fprintf(stderr, "error when closing ctree %d\n", ret);
	}

	close(devfd);

	return ret;

fail:
	return -1;

	
	// int fd = open(filename, O_CREAT, O_SYNC);
	// struct btrfs_fs_info *info;
	// struct btrfs_root *fsroot;
	// info = open_ctree_fs_info(devname, 0, 0, OPEN_CTREE_WRITES);
	// fsroot = root->fs_info->fs_root;

	// if (dir != NULL) {
	// 	fprintf(stdout, "dir type is %u\n", dir->type);
	// }

	// leaf = path.nodes[0];
	// inode = btrfs_item_ptr(leaf, path.slots[0], struct btrfs_inode_item);

	// if (inode == NULL) {
	// 	fprintf(stderr, "unable to obtain inode for %s\n", filename);
	// 	goto fail;
	// }
	// fprintf(stdout, "%llu\n", inode->generation);


	// ret = btrfs_insert_inode(trans, root, objectid, &btrfs_inode);
}

static void print_usage(void)
{
	printf("usage: btrfs-compose [-d] [-i] device mount_dir file [target device] [target offset]\n");
	printf("\t-d disable data checksum\n");
	printf("\t-i ignore xattrs and ACLs\n");
}

int main(int argc, char *argv[])
{
	int ret, fd;
	int noxattr = 0;
	int datacsum = 1;
	char *file_name, *full_path;
	char *device;
	char *mount_dir;
	char *tgt_dev = NULL;	
	unsigned long long tgt_extent_start = 0;
	unsigned long long num_bytes = 0;
	
	while(1) {
		int c = getopt(argc, argv, "dinr");
		if (c < 0)
			break;
		switch(c) {
			case 'd':
				datacsum = 0;
				break;
			case 'i':
				noxattr = 1;
				break;
			default:
				print_usage();
				return 1;
		}
	}
	argc = argc - optind;
	if (argc < 3) {
		print_usage();
		return 1;
	}

	device = argv[optind];
	mount_dir = argv[optind+1];
	file_name = argv[optind+2];
	if (argc > 3) {
		tgt_dev = argv[optind+3];
		tgt_extent_start = atoll(argv[optind+4]);
		num_bytes = atoll(argv[optind+5]);
	}
	full_path = (char *)malloc(strlen(mount_dir) + strlen(file_name));
	strcpy(full_path, mount_dir);
	strcat(full_path, file_name);

	ret = check_mounted(device);
	if (ret < 0) {
		fprintf(stderr, "Could not check mount status: %s\n",
			strerror(-ret));
		goto failed;
	} else if (ret == 0) {
		fprintf(stdout, "%s is not mounted\n", device);
		if (mount(device, mount_dir, "btrfs", MS_NOATIME, NULL)) {
			fprintf(stderr, "%s cannot be mounted\n", device);
			perror("Device cannot be mounted");
			goto failed;
		}
	} 

	fd = open(full_path, O_CREAT, O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "File %s cannot be created\n", file_name);
		goto failed;
	}
	close(fd);
	if (umount(mount_dir)) {
		perror("Device cannot be unmounted");
		goto failed;
	}

	ret = do_compose(device, file_name, tgt_dev, tgt_extent_start, 
		num_bytes, datacsum, noxattr);

	if (ret) {
		fprintf(stderr, "compose returned %d\n", ret);
		goto failed;
	}
	
failed:
	free(full_path);
	close(fd);
	mount(device, mount_dir, "btrfs", MS_NOATIME, NULL);
	return ret;
}
