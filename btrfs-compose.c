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
#include <fcntl.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include "ctree.h"
#include "disk-io.h"
#include "volumes.h"
#include "transaction.h"
#include "crc32c.h"
#include "utils.h"
#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2fs.h>
#include <ext2fs/ext2_ext_attr.h>

static int do_compose(const char *devname, const char *filename, 
	int datacsum, int noxattr)
{
	
	fprintf(stdout, "Creating file %s\n", filename);
	char *harddevname = "/dev/sdb";
	// char *hardfilename = "vm-image";
	char *hardfilename = "composed-file";

	// int fd = open(filename, O_CREAT, O_SYNC);

	struct btrfs_inode_item *inode;
	int devfd = open(harddevname, O_RDWR);
	int ret = 0;
	struct btrfs_path path;
	struct btrfs_dir_item *dir;
	// struct btrfs_fs_info *info;
	struct btrfs_root *root;
	// struct btrfs_root *fsroot;
	u64 root_dir, total_bytes, size, objectid;
	struct extent_buffer *leaf;
	struct btrfs_trans_handle *trans;
	struct btrfs_key key;
	struct btrfs_file_extent_item *fi;
	u64 offset;

	if (devfd < 0) {
		fprintf(stderr, "unable to open %s\n", devname);
		goto fail;
	}

	// info = open_ctree_fs_info(harddevname, 0, 0, OPEN_CTREE_WRITES);
	root = open_ctree_fd(devfd, harddevname, 0, OPEN_CTREE_WRITES);
	// fsroot = root->fs_info->fs_root;
	btrfs_init_path(&path);
	root_dir = btrfs_root_dirid(&root->fs_info->fs_root->root_item);
	


	// char *buf = malloc(1048576);
	// memset(buf, 'z', 1048576);
	// write(fd, buf, 1048576);
	// fsync(fd);
	// close(fd);

	if (root != NULL) {
		fprintf(stdout, "fs ID is %u, last alloc inode %llu\n", root->fs_info->fsid[0], 
			root->fs_info->fs_root->last_inode_alloc);
	}
	dir = btrfs_lookup_dir_item(NULL, root, &path,
		root_dir, hardfilename, strlen(hardfilename), 0);
	
	if (dir == NULL || IS_ERR(dir)) {
		fprintf(stderr, "unable to find file %s\n", hardfilename);
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
	fprintf(stdout, "old generation is %llu\n", btrfs_inode_generation(leaf, inode));
	btrfs_release_path(&path);
	key.objectid = objectid;
	key.offset = 0;
	btrfs_set_key_type(&key, BTRFS_EXTENT_DATA_KEY);
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);

	if (ret != 0) {
		fprintf(stderr, "unable to find first file extent\n");
		btrfs_release_path(&path);
	}

	leaf = path.nodes[0];
	btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
	fi = btrfs_item_ptr(leaf, path.slots[0],
			    struct btrfs_file_extent_item);
	offset = btrfs_file_extent_disk_bytenr(leaf, fi);
	fprintf(stdout, "first extent offset %llu\n", offset);

	// return ret;

	trans = btrfs_start_transaction(root, 1);
	if (!trans) {
		return -ENOMEM;
	}
	btrfs_set_inode_generation(leaf, inode, 7);
	fprintf(stdout, "new generation is %llu\n", btrfs_inode_generation(leaf, inode));
	fprintf(stdout, "objectid is %llu\n", key.objectid);
	ret = btrfs_insert_file_extent(trans, root, objectid, size, offset,
						1048576, 1048576);
	btrfs_set_inode_nbytes(leaf, inode, total_bytes + 1048576);
	btrfs_set_inode_size(leaf, inode, size + 1048576);
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
	BUG_ON(ret);

	close(devfd);

	// if (dir != NULL) {
	// 	fprintf(stdout, "dir type is %u\n", dir->type);
	// }

	// leaf = path.nodes[0];
	// inode = btrfs_item_ptr(leaf, path.slots[0], struct btrfs_inode_item);

	// if (inode == NULL) {
	// 	fprintf(stderr, "unable to obtain inode for %s\n", hardfilename);
	// 	goto fail;
	// }
	// fprintf(stdout, "%llu\n", inode->generation);


	// ret = btrfs_insert_inode(trans, root, objectid, &btrfs_inode);
	
	return ret;

fail:
	return -1;
}

static void print_usage(void)
{
	printf("usage: btrfs-compose [-d] [-i] device file\n");
	printf("\t-d disable data checksum\n");
	printf("\t-i ignore xattrs and ACLs\n");
}

int main(int argc, char *argv[])
{
	int ret;
	int noxattr = 0;
	int datacsum = 1;
	char *file;
	char *device;
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
	if (argc != 2) {
		print_usage();
		return 1;
	}

	device = argv[optind];
	file = argv[optind+1];

	ret = check_mounted(device);
	if (ret < 0) {
		fprintf(stderr, "Could not check mount status: %s\n",
			strerror(-ret));
		return 1;
	} else if (ret) {
		fprintf(stderr, "%s is mounted\n", device);
		return 1;
	}

	ret = do_compose(device, file, datacsum, noxattr);

	if (ret) {
		fprintf(stderr, "compose returned %d\n", ret);
		return 1;
	}
	return 0;
}
