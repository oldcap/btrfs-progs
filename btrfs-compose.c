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
	// struct btrfs_inode_item btrfs_inode;
	printf("Creating file %s\n", filename);
	int fd = open(filename, O_CREAT, O_SYNC);
	int devfd = open(devname, O_RDONLY);
	int ret;
	struct btrfs_path path;
	struct btrfs_dir_item *dir;
	struct btrfs_root *root;
	u64 root_dir;

	root = open_ctree_fd(devfd, devname, 0, OPEN_CTREE_WRITES);	
	btrfs_init_path(&path);
	root_dir = btrfs_root_dirid(&root->root_item);
	dir = btrfs_lookup_dir_item(NULL, root, &path,
				   root_dir, name, strlen(name), 0);
	
	// struct btrfs_root *root;
	// struct btrfs_trans_handle *trans;
	// u64 objectid;
	// u64 super_bytenr;
	
	// root = open_ctree_fd(fd, filename, super_bytenr, OPEN_CTREE_WRITES);
	// trans = btrfs_start_transaction(root, 1);

	// ret = btrfs_insert_inode(trans, root, objectid, &btrfs_inode);
	close(fd);
	return ret;
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
		fprintf(stderr, "%s is mounted\n", file);
		return 1;
	}

	ret = do_compose(file, datacsum, noxattr);

	if (ret)
		return 1;
	return 0;
}
