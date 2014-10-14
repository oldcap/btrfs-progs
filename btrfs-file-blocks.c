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

static int do_file_blocks(const char *devname, const char *filename)
{
	struct btrfs_inode_item *inode;
	int devfd;
	int ret = 0;
	struct btrfs_path path;
	struct btrfs_dir_item *dir;
	struct btrfs_root *root;	
	u64 root_dir, total_bytes, size, objectid;
	struct extent_buffer *leaf;
	struct btrfs_trans_handle *trans;
	u64 file_offset, disk_addr, extent_size;
	struct btrfs_chunk *chunk;
	struct btrfs_fs_info *info;
	struct btrfs_root *chunk_root;
	struct btrfs_key key, chunk_key;
	struct btrfs_block_group_cache *cache;
	struct btrfs_file_extent_item *fi;

	fprintf(stdout, "Checking blocks for file %s\n", filename);

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

	fprintf(stdout, "objectid is %llu\n", key.objectid);
	leaf = path.nodes[0];
	inode = btrfs_item_ptr(leaf, path.slots[0], struct btrfs_inode_item);
	total_bytes = btrfs_inode_nbytes(leaf, inode);
		// total_bytes = btrfs_stack_inode_nbytes(inode);
	size = btrfs_inode_size(leaf, inode);
	fprintf(stdout, "total bytes %llu, size is %llu\n", total_bytes, size);
	btrfs_release_path(&path);
	key.objectid = objectid;

	key.offset = 0;
	btrfs_set_key_type(&key, BTRFS_EXTENT_DATA_KEY);
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);

	if (ret != 0) {
		fprintf(stderr, "unable to find first file extent\n");
		btrfs_release_path(&path);
		goto fail;
	}

	for (file_offset = 0; file_offset < total_bytes; ) {
		fprintf(stdout, "file_offset=%d\n", file_offset);
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret != 0)
				break;	
			continue;
		}

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.objectid != objectid || key.offset != file_offset ||
			btrfs_key_type(&key) != BTRFS_EXTENT_DATA_KEY)
			break;

		fi = btrfs_item_ptr(leaf, path.slots[0],
			struct btrfs_file_extent_item);
		if (btrfs_file_extent_type(leaf, fi) != BTRFS_FILE_EXTENT_REG)
			break;
		if (btrfs_file_extent_compression(leaf, fi) ||
			btrfs_file_extent_encryption(leaf, fi) ||
			btrfs_file_extent_other_encoding(leaf, fi))
			break;

		disk_addr = btrfs_file_extent_disk_bytenr(leaf, fi);
		extent_size = btrfs_file_extent_disk_num_bytes(leaf, fi);

		cache = btrfs_lookup_block_group(root->fs_info, disk_addr);
		BUG_ON(!cache);
		chunk_key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
		chunk_key.offset = cache->key.objectid;
		chunk_key.type = BTRFS_CHUNK_ITEM_KEY;

		btrfs_release_path(&path);
		ret = btrfs_search_slot(NULL, chunk_root, &chunk_key, &path, 0, 0);
		if (ret != 0) {
			fprintf(stderr, "unable to find chunk\n");
			btrfs_release_path(&path);
			goto fail;
		}

		leaf = path.nodes[0];
		chunk = btrfs_item_ptr(leaf, path.slots[0],
			struct btrfs_chunk);

		int num_stripes = btrfs_chunk_num_stripes(leaf, chunk);
		int i;
		unsigned long long offset_in_chunk = disk_addr - cache->key.objectid;
		int last_stripe_in_offset = (int)(offset_in_chunk / BTRFS_STRIPE_LEN) % num_stripes;
		unsigned long long offset_in_stripe;

		fprintf(stdout, "extent file offset %llu, disk address %llu, size %llu, "
			"offset in chunk %llu, last stripe is %d", 
			file_offset, disk_addr, extent_size, offset_in_chunk,
			last_stripe_in_offset);

		if (num_stripes == 1) {
			fprintf(stdout, ", devid %llu offset %llu\n", 
				(unsigned long long)btrfs_stripe_devid_nr(leaf, chunk, 0),
				(unsigned long long)btrfs_stripe_offset_nr(leaf, chunk, 0) + 
				offset_in_chunk);
		} else {
			fprintf(stdout, ":\n");
			for (i = 0 ; i < num_stripes ; i++) {
				if (i < last_stripe_in_offset) {
					offset_in_stripe = (int)(offset_in_chunk / (BTRFS_STRIPE_LEN * num_stripes)) 
					* BTRFS_STRIPE_LEN + BTRFS_STRIPE_LEN;
				} else if (i == last_stripe_in_offset) {
					offset_in_stripe = (int)(offset_in_chunk / (BTRFS_STRIPE_LEN * num_stripes)) 
					* BTRFS_STRIPE_LEN + (offset_in_chunk % BTRFS_STRIPE_LEN);
				} else {
					offset_in_stripe = (int)(offset_in_chunk / (BTRFS_STRIPE_LEN * num_stripes)) 
					* BTRFS_STRIPE_LEN;
				}

				fprintf(stdout, "\tstripe %d devid %llu chunk offset %llu offset %llu\n", i,
					(unsigned long long)btrfs_stripe_devid_nr(leaf, chunk, i),
					(unsigned long long)btrfs_stripe_offset_nr(leaf, chunk, i),
					(unsigned long long)btrfs_stripe_offset_nr(leaf, chunk, i) + 
					offset_in_stripe);
			}
		}

		file_offset += btrfs_file_extent_num_bytes(leaf, fi);
		path.slots[0]++;
	}

	ret = close_ctree(root);
	if (ret) {
		fprintf(stderr, "error when closing ctree %d\n", ret);
	}

	close(devfd);

	return ret;

	fail:
	return -1;

// 	int ret = 0;
// 	fprintf(stdout, "Checking blocks for file %s\n", filename);
// 	struct btrfs_inode_item *inode;
// 	struct btrfs_chunk *chunk;
// 	int devfd;
// 	struct btrfs_root *root;
// 	struct btrfs_fs_info *info;
// 	struct btrfs_root *chunk_root;
// 	struct btrfs_path path;
// 	struct btrfs_dir_item *dir;
// 	u64 root_dir, total_bytes, size, objectid;
// 	struct extent_buffer *leaf;
// 	struct btrfs_key key, chunk_key;
// 	struct btrfs_block_group_cache *cache;
// 	struct btrfs_file_extent_item *fi;
// 	u64 file_offset, disk_addr, extent_size;
// 	char *dir_name;

// 	devfd = open(devname, O_RDONLY);
	
// 	if (devfd < 0) {
// 		fprintf(stderr, "unable to open %s\n", devname);
// 		goto fail;
// 	}

// 	root = open_ctree_fd(devfd, devname, 0, 
// 		OPEN_CTREE_PARTIAL);
// 	info = root->fs_info;
// 	chunk_root = info->chunk_root;

// 	btrfs_init_path(&path);
// 	root_dir = btrfs_root_dirid(&root->fs_info->fs_root->root_item);
// 	objectid = root_dir;

// 	if (root != NULL) {
// 		fprintf(stdout, "fs ID is %u\n", root->fs_info->fsid[0]);
// 	}

// 	char dup_filename[128];
// 	strcpy(dup_filename, filename);
// 	dir_name = strtok(dup_filename,"/");
// 	dir_name = strtok (NULL, "/");

// 	while (dir_name != NULL) {
// 		dir = btrfs_lookup_dir_item(NULL, root, &path,
// 			root_dir, dir_name, strlen(dir_name), 0);
// 		if (dir == NULL || IS_ERR(dir)) {
// 			fprintf(stderr, "unable to find path %s\n", dir_name);
// 			goto fail;
// 		}
// 		leaf = path.nodes[0];
// 		btrfs_dir_item_key_to_cpu(leaf, dir, &key);
// 		btrfs_release_path(&path);
// 		objectid = key.objectid;
// 		fprintf(stdout, "found path %s dir key %llu\n", 
// 			dir_name, key.objectid);

// 		root_dir = key.objectid;
// 		dir_name = strtok (NULL, "/");	
// 	}

// 	ret = btrfs_lookup_inode(NULL, root, &path, &key, 0);
// 	fprintf(stdout, "found inode key %llu\n", key.objectid);

// 	if (ret) {
// 		fprintf(stderr, "unable to find inode item\n");
// 		goto fail;
// 	}

// 	leaf = path.nodes[0];
// 	inode = btrfs_item_ptr(leaf, path.slots[0], struct btrfs_inode_item);
// 	total_bytes = btrfs_inode_nbytes(leaf, inode);
// 	// total_bytes = btrfs_stack_inode_nbytes(inode);
// 	size = btrfs_inode_size(leaf, inode);
// 	fprintf(stdout, "total bytes %llu, size is %llu\n", total_bytes, size);
// 	fprintf(stdout, "generation is %llu\n", btrfs_inode_generation(leaf, inode));
// 	btrfs_release_path(&path);
// 	key.objectid = objectid;

// 	key.offset = 0;
// 	btrfs_set_key_type(&key, BTRFS_EXTENT_DATA_KEY);
// 	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);

// 	if (ret != 0) {
// 		fprintf(stderr, "unable to find first file extent\n");
// 		btrfs_release_path(&path);
// 		goto fail;
// 	}

// 	for (file_offset = 0; file_offset < total_bytes; ) {
// 		leaf = path.nodes[0];
// 		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
// 			ret = btrfs_next_leaf(root, &path);
// 			if (ret != 0)
// 				break;	
// 			continue;
// 		}

// 		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
// 		if (key.objectid != objectid || key.offset != file_offset ||
// 		    btrfs_key_type(&key) != BTRFS_EXTENT_DATA_KEY)
// 			break;

// 		fi = btrfs_item_ptr(leaf, path.slots[0],
// 				    struct btrfs_file_extent_item);
// 		if (btrfs_file_extent_type(leaf, fi) != BTRFS_FILE_EXTENT_REG)
// 			break;
// 		if (btrfs_file_extent_compression(leaf, fi) ||
// 		    btrfs_file_extent_encryption(leaf, fi) ||
// 		    btrfs_file_extent_other_encoding(leaf, fi))
// 			break;

// 		disk_addr = btrfs_file_extent_disk_bytenr(leaf, fi);
// 		extent_size = btrfs_file_extent_disk_num_bytes(leaf, fi);

// 		cache = btrfs_lookup_block_group(root->fs_info, disk_addr);
// 		BUG_ON(!cache);
// 		chunk_key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
// 		chunk_key.offset = cache->key.objectid;
// 		chunk_key.type = BTRFS_CHUNK_ITEM_KEY;

// 		btrfs_release_path(&path);
// 		ret = btrfs_search_slot(NULL, chunk_root, &chunk_key, &path, 0, 0);
// 		if (ret != 0) {
// 			fprintf(stderr, "unable to find chunk\n");
// 			btrfs_release_path(&path);
// 			goto fail;
// 		}

// 		leaf = path.nodes[0];
// 		chunk = btrfs_item_ptr(leaf, path.slots[0],
// 					struct btrfs_chunk);

// 		int num_stripes = btrfs_chunk_num_stripes(leaf, chunk);
// 		int i;
// 		unsigned long long offset_in_chunk = disk_addr - cache->key.objectid;
// 		int last_stripe_in_offset = (int)(offset_in_chunk / BTRFS_STRIPE_LEN) % num_stripes;
// 		unsigned long long offset_in_stripe;

// 		fprintf(stdout, "extent file offset %llu, disk address %llu, size %llu, "
// 			"offset in chunk %llu, last stripe is %d", 
// 			file_offset, disk_addr, extent_size, offset_in_chunk,
// 			last_stripe_in_offset);

// 		if (num_stripes == 1) {
// 			fprintf(stdout, ", devid %llu offset %llu\n", 
// 				(unsigned long long)btrfs_stripe_devid_nr(leaf, chunk, 0),
// 				(unsigned long long)btrfs_stripe_offset_nr(leaf, chunk, 0) + 
// 				offset_in_chunk);
// 		} else {
// 			fprintf(stdout, ":\n");
// 			for (i = 0 ; i < num_stripes ; i++) {
// 				if (i < last_stripe_in_offset) {
// 					offset_in_stripe = (int)(offset_in_chunk / (BTRFS_STRIPE_LEN * num_stripes)) 
// 						* BTRFS_STRIPE_LEN + BTRFS_STRIPE_LEN;
// 				} else if (i == last_stripe_in_offset) {
// 					offset_in_stripe = (int)(offset_in_chunk / (BTRFS_STRIPE_LEN * num_stripes)) 
// 						* BTRFS_STRIPE_LEN + (offset_in_chunk % BTRFS_STRIPE_LEN);
// 				} else {
// 					offset_in_stripe = (int)(offset_in_chunk / (BTRFS_STRIPE_LEN * num_stripes)) 
// 						* BTRFS_STRIPE_LEN;
// 				}

// 				fprintf(stdout, "\tstripe %d devid %llu chunk offset %llu offset %llu\n", i,
// 					(unsigned long long)btrfs_stripe_devid_nr(leaf, chunk, i),
// 					(unsigned long long)btrfs_stripe_offset_nr(leaf, chunk, i),
// 					(unsigned long long)btrfs_stripe_offset_nr(leaf, chunk, i) + 
// 					offset_in_stripe);
// 			}
// 		}

// 		file_offset += btrfs_file_extent_num_bytes(leaf, fi);
// 		path.slots[0]++;
// 	}

// 	return ret;

// fail:
// 	return -1;
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
		fprintf(stderr, "file blocks returned %d\n", ret);
	}
	return ret;
}
