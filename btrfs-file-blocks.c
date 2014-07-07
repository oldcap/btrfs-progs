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

static int do_file_blocks(const char *devname, const char *filename)
{
	int ret = 0;
	fprintf(stdout, "Checking blocks for file %s\n", filename);
	struct btrfs_inode_item *inode;
	int devfd;
	struct btrfs_root *root;
	struct btrfs_path path;
	struct btrfs_dir_item *dir;
	u64 root_dir, total_bytes, size, objectid;;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	struct btrfs_file_extent_item *fi;
	u64 file_offset, disk_addr, extent_size;

	devfd = open(devname, O_RDONLY);
	
	if (devfd < 0) {
		fprintf(stderr, "unable to open %s\n", devname);
		goto fail;
	}

	root = open_ctree_fd(devfd, devname, 0, 
		OPEN_CTREE_PARTIAL);
	btrfs_init_path(&path);
	root_dir = btrfs_root_dirid(&root->fs_info->fs_root->root_item);

	if (root != NULL) {
		fprintf(stdout, "fs ID is %u\n", root->fs_info->fsid[0]);
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
	file_offset = btrfs_file_extent_offset(leaf, fi);
	disk_addr = btrfs_file_extent_disk_bytenr(leaf, fi);
	extent_size = btrfs_file_extent_disk_num_bytes(leaf, fi);
	fprintf(stdout, "extent file offset %llu, disk address %llu, size %llu\n", 
		file_offset, disk_addr, extent_size);

	return ret;

fail:
	return -1;
}

static void print_usage(void)
{
	printf("usage: btrfs-file-blocks device file\n");
}

int main(int argc, char *argv[])
{
	int ret;
	char *file;
	char *device;

	if (argc != 3) {
		print_usage();
		return 1;
	}

	device = argv[1];
	file = argv[2];

	ret = do_file_blocks(device, file);

	if (ret) {
		fprintf(stderr, "compose returned %d\n", ret);
	}
	return ret;
}