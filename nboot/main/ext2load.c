
/* refs for ext2fs : 
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
		printf("Partition %d : incompatible ext2/3/4 features (%hx)\n", part_num, feat_incompat);
		return(-1);
	}
	uint32_t log_blksz=INAT(uint32_t, buf, 0x18); 
	sb->block_size=(1 << (2+log_blksz));
	if(sb->block_size>8) {
		printf("Partition %d : block size (%d) larger than 4096\n", part_num, sb->blocksize*512);
		return(-1);
	}
	sb->inode_size=INAT(uint16_t, buf, 0x58); 
	sb->inodes_per_group=INAT(uint32_t, buf, 0x28); 
	sb->blocks_count=INAT(uint32_t, buf, 0x4); 
	sb->blocks_per_group=INAT(uint32_t, buf, 0x20); 
	printf("Partition %d : ext2, %d blocks, block size %d, inode size %d, %d inodes per group\n", 
						part_num, sb->blocks_count, sb->block_size*512, sb->inode_size, sb->inodes_per_group);

	return(0);
}

int ext2_read_block(struct ext2_sb *sb, uint32_t block_num, char *buf) {
	printf(" (read block %d)\n", block_num);
	int rc;
	if((rc=mmc_bread(SDC_NO, sb->part_offset+block_num*sb->block_size, sb->block_size, buf))<0) {
		printf("read block %d failed\n", block_num);
		return(-1);
	}
}

void ext2_read_bgtable(struct ext2_sb *sb) {
	/* cache block group descriptor table into the sb struct */
	uint32_t bg_count=(sb->blocks_count+sb->blocks_per_group-1)/sb->blocks_per_group;
	uint32_t bgtable_size=32*bg_count; /* bgtable size in bytes, each block grp descriptor is 32B */
	sb->bg_table=malloc(bgtable_size);
	bgtable_size=bgtable_size/(512*sb->block_size); /* number of blocks for the bgtable */
	/* if blocksize>1024B : superblock is at block 0+1024B, first block group descriptor in block 1+0B */
	/* if blocksize==1024B: superblock is at block 1+0B, first block group descriptor in block 2+0B */
	uint32_t bgtable_begin=(sb->block_size > 2 ? 1 : 2);
	printf("%d block groups, block group descriptor table has %d blocks starting from block %d\n", 
			bg_count, bgtable_size, bgtable_begin);
	for(int bl=bgtable_begin; bl<bgtable_begin+bgtable_size; bl++) 
		ext2_read_block(sb, bl, sb->bg_table+512*sb->block_size*bl);
}

void ext2_read_inode_block_map(struct ext2_sb *sb, int inode_num, uint32_t *bmap) {
	/* read inode table of the block group of the inode */
	char *tmp=malloc(512*sb->block_size);
	uint32_t bg_of_inode=(inode_num-1)/sb->inodes_per_group;
	uint32_t inode_table_block_nr=INAT(uint32_t, sb->bg_table, 32*bg_of_inode+0x8);
	ext2_read_block(sb, inode_table_block_nr, tmp);

	/** TODO XXX XXX XXX **/
	
	/* add offset into inode table, plus offset of inode.i_block (0x28) */
	uint32_t off_into_bg_inode_table=((inode_num-1)%sb->inodes_per_group) + 0x28;

	/* copy block map */
	memcpy((char*)bmap, tmp+off_into_bg_inode_table, 60);

	free(tmp);
}

/* reads at most bcount blocks whose numbers are in NULL-terminated blist, 
 * returns number of blocks effectively read */
int ext2_read_block_list(struct ext2_sb *sb, uint32_t *blist, int bcount, char *dest) {
	int i;
	for(i=0; i<bcount; i++) {
		if(bmap[i]==0) break;
		ext2_read_block(sb, blist[i], dest+i*sb->block_size*512);
	}
	return(i);
}

/* reads at most max_block_count into dest, from the blist found at block addr */
int ext2_read_bmap_ind(struct ext2_sb *sb, uint32_t addr, int max_block_count, char *dest) {
	char *tmp=malloc(512*sb->block_size);
	ext2_read_block(sb, addr, tmp);
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
	ext2_read_inode_block_map(sb, inode_num, bmap);
	return ext2_read_bmap_contents(sb, bmap, max_block_count, dest);
}


int load_ext2(phys_addr_t *uboot_base, phys_addr_t *optee_base, \
		phys_addr_t *monitor_base, phys_addr_t *rtos_base, \
		phys_addr_t *opensbi_base, phys_addr_t *dtb_base, char **cmdline) {
	int rc;
	int part_num;
	char *buf;
	struct ext2_sb sb;


	buf=malloc(4096);
	*optee_base=*monitor_base=*rtos_base=0;
	*cmdline=NULL;

/* 
	if((rc=sunxi_mmc_init(SDC_NO, 4, BT0_head.prvt_head.storage_gpio, 16))<0)
		return(rc);

	printf("mmc_init ok\n");
*/

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
		if(ext2_sb_read(buf, part_num, &sb)>=0) 
			break;
	}
	if(part_num==3) {
		printf("No suitable partition found\n");
		return(-1);
	}

	while(1);

	return(0);
}
