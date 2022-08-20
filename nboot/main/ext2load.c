/*
 *    Copyright (C) 2022 Alexandre Janon <alex14fr@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>. 
 *
 */

/* references for ext2fs : 
 * https://www.kernel.org/doc/html/latest/filesystems/ext4/globals.html
 * https://www.kernel.org/doc/html/latest/filesystems/ext4/dynamic.html
 */

#include <common.h>
#include <private_boot0.h>
#include <spare_head.h>
#include <mmc_boot0.h>
extern const boot0_file_head_t  BT0_head;

#define SDC_NO 0   /* number of SD Card */

#define INAT(type, ptr, offset) *((type *)(ptr+offset))

struct ext2_sb {
	uint32_t part_offset; /* in sectors (1 sector=512 bytes) */
	uint16_t block_size; /* in sectors */
	uint16_t inode_size; /* in bytes */
	uint32_t inodes_per_group;
	uint32_t blocks_count;
	uint32_t blocks_per_group;
	char *bg_table; /* block group descriptor table */
};

int ext2_sb_read(char *mbr, int part_num, char *buf, struct ext2_sb *sb) {
	/* find beginning of partition */
	char *part_entry=mbr+446+16*part_num;
	if(!(part_entry[0] & 0x80)) {
		printf("Partition %d : not bootable in MBR partition table\n", part_num);
		return(-1);
	}
	sb->part_offset=INAT(uint32_t, part_entry, 8); 

	/* read ext2 superblock : 2 sectors (1024 bytes) at offset part_offset+2 sectors */
	mmc_bread(SDC_NO, sb->part_offset+2, 2, buf);
	if(buf[0x38]!=0x53 || buf[0x39]!=0xEF) {
		printf("Partition %d : invalid ext2 magic number\n", part_num);
		return(-1);
	}
	uint32_t feat_incompat=INAT(uint32_t, buf, 0x60); 
	if(feat_incompat) {
		printf("Partition %d : incompatible ext2/3/4 features (%x)\n", part_num, feat_incompat);
		return(-1);
	}
	uint32_t log_blksz=INAT(uint32_t, buf, 0x18); 
	sb->block_size=(1 << (1+log_blksz));
	if(sb->block_size>8) {
		printf("Partition %d : block size (%d) larger than 4096\n", part_num, sb->block_size*512);
		return(-1);
	}
	sb->inode_size=INAT(uint16_t, buf, 0x58); 
	sb->inodes_per_group=INAT(uint32_t, buf, 0x28); 
	sb->blocks_count=INAT(uint32_t, buf, 0x4); 
	sb->blocks_per_group=INAT(uint32_t, buf, 0x20); 
	printf("Partition %d : ext2, %d blocks, block size %d, inode size %d, %d inodes per group, %d blocks per group\n", 
			part_num, sb->blocks_count, sb->block_size*512, sb->inode_size, sb->inodes_per_group, sb->blocks_per_group);

	return(0);
}

int ext2_read_block(struct ext2_sb *sb, uint32_t block_num, char *buf) {
	printf(" (read block %d)\n", block_num);
	int rc;
	if((rc=mmc_bread(SDC_NO, sb->part_offset+block_num*sb->block_size, sb->block_size, buf))<0) {
		printf("read block %d failed\n", block_num);
		return(-1);
	}
	return(rc);
}

/* cache block group descriptor table into the sb struct */
void ext2_read_bgtable(struct ext2_sb *sb) {
	/* figure out how many blocks are occupied by block group descriptor table */
	uint32_t bg_count=(sb->blocks_count+sb->blocks_per_group-1)/sb->blocks_per_group;
	uint32_t bgtable_size=32*bg_count; /* bgtable size in bytes, each block grp descriptor is 32B */
	sb->bg_table=malloc(bgtable_size);
	if(!sb->bg_table) {
		printf("malloc(%d bytes) for bgtable failed\n", bgtable_size);
	}
	bgtable_size=(bgtable_size+512*sb->block_size-1)/(512*sb->block_size); /* number of blocks for the bgtable */

	/* if blocksize>1024B : superblock is at block 0+1024B, first block group descriptor in block 1+0B */
	/* if blocksize==1024B: superblock is at block 1+0B, first block group descriptor in block 2+0B */
	uint32_t bgtable_begin=(sb->block_size > 2 ? 1 : 2);

	printf("%d block groups, block group descriptor table has %d block(s) starting from block %d\n", 
			bg_count, bgtable_size, bgtable_begin);

	for(uint32_t bl=0; bl<bgtable_size; bl++) 
		ext2_read_block(sb, bgtable_begin+bl, sb->bg_table+512*sb->block_size*bl);
}

/* reads the block map of inode inode_num in bmap */
/* tmp is a scratch workspace whose size is at least one block */
/* returns the size of the file, in bytes */
uint32_t ext2_read_inode_block_map(struct ext2_sb *sb, int inode_num, char *tmp, uint32_t *bmap) {
	/* read inode table of the block group of the inode */
	uint32_t bg_of_inode=(inode_num-1)/sb->inodes_per_group;
	uint32_t inode_table_block_nr=INAT(uint32_t, sb->bg_table, 32*bg_of_inode+0x8);
	printf("inode %d is in block group %d, inode table of this block group is at block %d\n", 
				inode_num, bg_of_inode, inode_table_block_nr);
	ext2_read_block(sb, inode_table_block_nr, tmp);

	/* add offset into inode table, plus offset of inode.i_block (0x28) */
	uint32_t off_into_bg_inode_table=sb->inode_size*((inode_num-1)%sb->inodes_per_group) + 0x28;

	/* copy block map */
	memcpy((char*)bmap, tmp+off_into_bg_inode_table, 60);

	printf("got block map: \n");
	for(int i=0; i<15; i++) printf("%d ", INAT(uint32_t, bmap, 4*i));
	printf("\n");

	uint32_t fsize=INAT(uint32_t, tmp, off_into_bg_inode_table-0x28+0x4); 
	printf("file size=%d\n", fsize);

	return(fsize);
}

/* reads at most bcount blocks whose numbers are in NULL-terminated blist, 
 * returns number of blocks effectively read */
int ext2_read_block_list(struct ext2_sb *sb, uint32_t *blist, int bcount, char *dest) {
	int i;
	for(i=0; i<bcount; i++) {
		if(blist[i]==0) break;
		ext2_read_block(sb, blist[i], dest+i*sb->block_size*512);
	}
	return(i);
}

/* reads at most max_block_count into dest, from the blist found at block addr */
int ext2_read_bmap_ind(struct ext2_sb *sb, uint32_t addr, int max_block_count, char *dest) {
	uint32_t *tmp=malloc(512*sb->block_size);
	ext2_read_block(sb, addr, (char*)tmp);
	int max_block_addr_in_block=(512*sb->block_size)/4;
	int nread=ext2_read_block_list(sb, tmp, 
		(max_block_count>max_block_addr_in_block ? max_block_addr_in_block : max_block_count), dest);
	free(tmp);
	return(nread);
}

/* reads from double-indirect addr in block map */
int ext2_read_bmap_dblind(struct ext2_sb *sb, uint32_t addr, int max_block_count, char *dest) {
	uint32_t *tmp=malloc(512*sb->block_size);
	ext2_read_block(sb, addr, (char*)tmp);
	int max_block_addr_in_block=(512*sb->block_size)/4;
	int blocks_read=0;
	for(int i=0; i<max_block_addr_in_block && blocks_read<=max_block_count; i++) {
		blocks_read += ext2_read_bmap_ind(sb, tmp[i], max_block_count-blocks_read, dest+blocks_read*512*sb->block_size);
	}
	free(tmp);
	return(blocks_read);
}

/* read contents (at most maxBlockNumber blocks) of an inode given its 60-byte block map */
/* returns number of blocks effectively read */
int ext2_read_bmap_contents(struct ext2_sb *sb, uint32_t *bmap, int max_block_count, char *dest) {
	int rc;
	int blocks_read;

	/* direct blocks */
	rc=ext2_read_block_list(sb, bmap, (max_block_count>12 ? 12 : max_block_count), dest);
	blocks_read=rc;
	max_block_count-=rc;
	if(max_block_count<=0 || !bmap[12]) return(blocks_read);

	/* indirect block */
	rc=ext2_read_bmap_ind(sb, bmap[12], max_block_count, dest+blocks_read*512*sb->block_size);
	blocks_read+=rc;
	max_block_count-=rc;
	if(max_block_count<=0 || !bmap[13]) return(blocks_read);

	/* double-indirect block */
	rc=ext2_read_bmap_dblind(sb, bmap[13], max_block_count, dest+blocks_read*512*sb->block_size);
	blocks_read+=rc;
	max_block_count-=rc;
	if(max_block_count<=0 || !bmap[14]) return(blocks_read);

	/* triple-indirect block */
	if(bmap[14]) {
		printf("Warning: file truncated (triple-indirect block map not supported)\n");
	}

	return(blocks_read);
}

/* reads at most max_block_count blocks of the data whose inode is inode_num */
int ext2_read_inode_contents(struct ext2_sb *sb, uint32_t inode_num, int max_block_count, char *dest) {
	uint32_t bmap[60];
	uint32_t fsize=ext2_read_inode_block_map(sb, inode_num, dest, bmap); // use dest as scratch workspace
	int block_count=(fsize+512*sb->block_size-1)/(512*sb->block_size);
	if(max_block_count<block_count) {
		printf("Warning: block_count of file (%d) is larger than max_block_count (%d); file will be truncated\n", block_count, max_block_count);
	} else {
		max_block_count=block_count;
		printf("max_block_count set to %d\n", block_count);
	}
	return ext2_read_bmap_contents(sb, bmap, max_block_count, dest);
}

/* returns inode number from filename and (linear) directory entry */
uint32_t ext2_inode_num(struct ext2_sb *sb, char *filename, uint16_t name_len, char *dirent, int dirent_size) {
	int idx=0;
	while(idx<dirent_size) {
		uint16_t c_name_len=INAT(uint16_t, dirent, idx+0x6);
		if(c_name_len != name_len) {
			printf("c_name_len=%d, mismatch %d\n", c_name_len, name_len);
		} else {
			if(memcmp(filename, dirent+idx+0x8, name_len)==0) {
				uint32_t inode_num=INAT(uint32_t, dirent, idx);
				printf("%s is at inode %d\n", filename, inode_num);
				return(inode_num);
			} else { 
				printf("name mismatch\n"); 
			}
		}
		printf("idx before=%d\n",idx);
		idx+=INAT(uint16_t, dirent, idx+0x4);
		printf("idx after=%d\n",idx);
	}
	return(0); // not found
}

/* main function */
int load_ext2(phys_addr_t *uboot_base, phys_addr_t *optee_base, \
		phys_addr_t *monitor_base, phys_addr_t *rtos_base, \
		phys_addr_t *opensbi_base, phys_addr_t *dtb_base, char **cmdline) {
	int rc;
	int part_num;
	char buf[4096];
	struct ext2_sb sb;


	*optee_base=*monitor_base=*rtos_base=0;
	*cmdline=NULL;

	if((rc=sunxi_mmc_init(SDC_NO, 4, BT0_head.prvt_head.storage_gpio, 16))<0)
		return(rc);

	/* fetch MBR */
	if((rc=mmc_bread(SDC_NO, 0, 1, buf))<0) {
		printf("Error reading MBR\n");
		return(rc);
	}
	if(buf[510]!=0x55 || buf[511]!=0xAA) {
		printf("Invalid MBR signature\n");
		return(-1);
	}

	/* find an ext2 filesystem marked as bootable in part. table */
	for(part_num=0; part_num<3; part_num++) {
		if(ext2_sb_read(buf, part_num, buf+1024, &sb)>=0) 
			break;
	}
	if(part_num==3) {
		printf("No suitable partition found\n");
		return(-1);
	}

	/* read root directory (inode 2) */
	ext2_read_bgtable(&sb);
	char *rootdir=malloc(4096);
	uint32_t rootdir_size=ext2_read_inode_contents(&sb, 2, 1, rootdir);

	printf("rootdir size=%d\n", rootdir_size);
	for(int i=0;i<rootdir_size;i++) {
		printf("%x ", rootdir[i]);
	}

	uint32_t inum=ext2_inode_num(&sb, "hello.txt", 9, rootdir, rootdir_size);
	if(inum>0) {
		uint32_t fsize=ext2_read_inode_contents(&sb, inum, 1, buf);
		buf[fsize]=0;
		printf("read : %s\n", buf);
	} else {
		printf("file not found\n");
	}

	while(1);

	return(0);
}
