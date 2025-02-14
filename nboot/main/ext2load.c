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
#define BGT_SIZE 1024 /* maximal size of block group descriptor table (in bytes) */

#define INAT(type, ptr, offset) *((type *)(ptr+offset))

struct ext2_sb {
	uint32_t part_offset; /* in sectors (1 sector=512 bytes) */
	uint16_t block_size; /* in sectors */
	uint16_t inode_size; /* in bytes */
	uint32_t inodes_per_group;
	uint32_t blocks_count;
	uint32_t blocks_per_group;
//	char bg_table[BGT_SIZE]; /* block group descriptor table */
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
	if(sb->block_size>2) {
		printf("Partition %d : block size (%d) larger than 1024\n", part_num, sb->block_size*512);
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
	//printf(" (read block %d part_off=%d block_size=%d)\n", block_num, sb->part_offset, sb->block_size);
	int rc;
	if((rc=mmc_bread(SDC_NO, sb->part_offset+block_num*sb->block_size, sb->block_size, buf))<0) {
		printf("read block %d failed\n", block_num);
		return(-1);
	}
	return(rc);
}

#if 0
/* cache block group descriptor table into the sb struct */
/* for memory reasons max block size=max BGTable size=1024B, so max block groups=1024/32=32 */
/* with default blocks per group (8192) this gives a maximal FS size of 256MiB, */
/* use the -g parameter of mke2fs if you want more */
static void ext2_read_bgtable(struct ext2_sb *sb) {
	/* figure out how many blocks are occupied by block group descriptor table */
	uint32_t bg_count=(sb->blocks_count+sb->blocks_per_group-1)/sb->blocks_per_group;
	uint32_t bgtable_size=32*bg_count; /* bgtable size in bytes, each block grp descriptor is 32B */
	if(bgtable_size>BGT_SIZE) {
		printf("Warning: block group descriptor table size (%d) is larger than %d, truncating\n", bgtable_size, BGT_SIZE);
		bgtable_size=BGT_SIZE;
	}
	bgtable_size=1; //(bgtable_size+512*sb->block_size-1)/(512*sb->block_size); /* number of blocks for the bgtable */

	/* if blocksize>1024B : superblock is at block 0+1024B, first block group descriptor in block 1+0B */
	/* if blocksize==1024B: superblock is at block 1+0B, first block group descriptor in block 2+0B */
	uint32_t bgtable_begin=2;   //(sb->block_size > 2 ? 1 : 2);

	printf("%d block groups, block group descriptor table has %d block(s) starting from block %d\n", 
			bg_count, bgtable_size, bgtable_begin);

	for(uint32_t bl=0; bl<bgtable_size; bl++) 
		ext2_read_block(sb, bgtable_begin+bl, sb->bg_table+512*sb->block_size*bl);
}
#endif

/* read a block group descriptor into dest (32 bytes) */
/* tmp is a scratch of at least 512 bytes */
void ext2_get_bgdesc(struct ext2_sb *sb, uint32_t bg_num, char *tmp, char *dest) {
	printf("ext2_get_bgdesc: bg_num=%d\n", bg_num);
	/* if blocksize>1024B : superblock is at block 0+1024B, first block group descriptor in block 1+0B */
	/* if blocksize==1024B: superblock is at block 1+0B, first block group descriptor in block 2+0B */
	uint32_t bgtable_begin=2;   //(sb->block_size > 2 ? 1 : 2);
	uint32_t off_absolute=512*(sb->part_offset+bgtable_begin*sb->block_size)+32*bg_num; // in bytes
	uint32_t sector_number=off_absolute/512;
	uint16_t off_into_sector=off_absolute%512;
	printf("ext2_get_bgdesc: part_offset=%d block_size=%d bg_num=%d off_absolute=%d sector_number=%d off_into_sector=%d\n", 
			sb->part_offset, sb->block_size, bg_num, off_absolute, sector_number, off_into_sector);
	mmc_bread(SDC_NO, sector_number, 1, tmp);
	memcpy(dest, tmp+off_into_sector, 32);
}

/* read the block map of inode inode_num in bmap */
/* tmp is a scratch workspace whose size is at least one sector */
/* returns the size of the file, in bytes */
uint32_t ext2_read_inode_block_map(struct ext2_sb *sb, int inode_num, char *tmp, uint32_t *bmap) {
	/* get location of inode table of the block group of the inode */
	uint32_t bg_of_inode=(inode_num-1)/sb->inodes_per_group;
	ext2_get_bgdesc(sb, bg_of_inode, tmp+512, tmp);
	uint32_t inode_table_block_nr=INAT(uint32_t, tmp, 0x8);
	printf("inode %d is in block group %d, inode table of this block group starts at block %d\n", 
				inode_num, bg_of_inode, inode_table_block_nr);
	/* get location of this inode in its inode table */
	uint32_t off_into_bg_inode_table=sb->inode_size*((inode_num-1)%sb->inodes_per_group);
	printf("A=%d B=%d C=%d\n", sb->inode_size, inode_num, sb->inodes_per_group);
	uint32_t abs_inode=(sb->part_offset+inode_table_block_nr*sb->block_size)*512+off_into_bg_inode_table; /* in bytes */
	uint32_t sector_nr=abs_inode/512;
	uint32_t off_into_sector=abs_inode%512;
	printf("inode info at offset %d into inode table of block group = absolute byte %d, sector %d, off into sector %d \n", 
				off_into_bg_inode_table, abs_inode, sector_nr, off_into_sector);
	/* fetch the sector containing requested inode */
	mmc_bread(SDC_NO, sector_nr, 1, tmp);

	/* copy block map */
	memcpy((char*)bmap, tmp+off_into_sector+0x28, 60);
	printf("got block map: \n");
	for(int i=0; i<15; i++) printf("%d ", bmap[i]);
	printf("\n");

	/* get file size */
	uint32_t fsize=INAT(uint32_t, tmp, off_into_sector+0x4); 
	printf("file size=%d\n", fsize);

	return(fsize);
}

/* read at most bcount blocks whose numbers are in NULL-terminated blist, 
 * returns number of blocks effectively read */
int ext2_read_block_list(struct ext2_sb *sb, uint32_t *blist, int bcount, char *dest) {
	int i;
	for(i=0; i<bcount; i++) {
		if(blist[i]==0) break;
		ext2_read_block(sb, blist[i], dest+i*sb->block_size*512);
	}
	return(i);
}

/* read from double-indirect(level=2) or indirect(level=1) addr in block map */
/* tmp is a scratch holding at least (level) blocks */
int ext2_read_bmap_indirect(int level, struct ext2_sb *sb, uint32_t addr, int max_block_count, char *tmp, char *dest) {
	uint32_t *iblist=(uint32_t*)(tmp+1024*(level-1)); // blocksize<=1024 => number of block addr in a sector<=256
	ext2_read_block(sb, addr, (char*)iblist);
	int max_block_addr_in_block=(512*sb->block_size)/4;
	int blocks_read=0;
	if(level==1) {
		int blk_count=max_block_addr_in_block;
		if(blk_count>max_block_count) blk_count=max_block_count;
		blocks_read=ext2_read_block_list(sb, iblist, blk_count, dest);
		return(blocks_read);
	} else if(level>1) {
		for(int i=0; i<max_block_addr_in_block && blocks_read<=max_block_count; i++) {
			blocks_read += ext2_read_bmap_indirect(level-1, sb, iblist[i], max_block_count-blocks_read, tmp, dest+blocks_read*512*sb->block_size);
		}
		return(blocks_read);
	} else {
		printf("Shouldn't happen\n");
		return(0);
	}
}


/* read contents (at most maxBlockNumber blocks) of an inode given its 60-byte block map */
/* tmp is a scratch holding at least 2 blocks */
/* returns number of blocks effectively read */
int ext2_read_bmap_contents(struct ext2_sb *sb, uint32_t *bmap, int max_block_count, char *tmp, char *dest) {
	int rc;
	int blocks_read;

	/* direct blocks */
	rc=ext2_read_block_list(sb, bmap, (max_block_count>12 ? 12 : max_block_count), dest);
	blocks_read=rc;
	max_block_count-=rc;
	if(max_block_count<=0 || !bmap[12]) return(blocks_read);

	/* indirect block */
	rc=ext2_read_bmap_indirect(1, sb, bmap[12], max_block_count, tmp, dest+blocks_read*512*sb->block_size);
	blocks_read+=rc;
	max_block_count-=rc;
	if(max_block_count<=0 || !bmap[13]) return(blocks_read);

	/* double-indirect block */
	rc=ext2_read_bmap_indirect(2, sb, bmap[13], max_block_count, tmp, dest+blocks_read*512*sb->block_size);
	blocks_read+=rc;
	max_block_count-=rc;
	if(max_block_count<=0 || !bmap[14]) return(blocks_read);

	/* triple-indirect block */
	if(bmap[14]) {
		printf("Warning: file truncated, triple-indirect block map not supported)\n");
	}

	return(blocks_read);
}

/* reads at most max_block_count blocks of the data whose inode is inode_num */
/* tmp is a scratch of at least 60 bytes+2 blocks */
int ext2_read_inode_contents(struct ext2_sb *sb, uint32_t inode_num, int max_block_count, char *tmp, char *dest) {
	uint32_t *bmap=(uint32_t*)(tmp+2*512*sb->block_size);
	uint32_t fsize=ext2_read_inode_block_map(sb, inode_num, dest, bmap); // use dest as scratch
	int block_count=(fsize+512*sb->block_size-1)/(512*sb->block_size);
	if(max_block_count<block_count) {
		printf("Warning: block_count of file (%d) is larger than max_block_count (%d); file will be truncated\n", block_count, max_block_count);
	} else {
		max_block_count=block_count;
		printf("max_block_count set to %d\n", block_count);
	}
	ext2_read_bmap_contents(sb, bmap, max_block_count, tmp, dest);
	return(fsize);
}

/* returns inode number from filename and (linear) directory entry */
uint32_t ext2_inode_num(struct ext2_sb *sb, char *filename, uint16_t name_len, char *dirent, int dirent_size) {
	int idx=0;
	while(idx<dirent_size) {
		uint16_t c_name_len=INAT(uint16_t, dirent, idx+0x6);
		if(c_name_len != name_len) {
			//printf("c_name_len=%d, mismatch %d\n", c_name_len, name_len);
		} else {
			if(memcmp(filename, dirent+idx+0x8, name_len)==0) {
				uint32_t inode_num=INAT(uint32_t, dirent, idx);
				printf("%s is at inode %d\n", filename, inode_num);
				return(inode_num);
			} else { 
				//printf("name mismatch\n"); 
			}
		}
		uint16_t reclen=INAT(uint16_t, dirent, idx+0x4);
		idx+=reclen;
		//printf("reclen=%d, idx after=%d, dirent_size=%d\n", reclen, idx, dirent_size);
		if(reclen==0) break;
	}
	return(0); // not found
}

#define LOAD_SCRATCH  0x01010000
#define LOAD_SCRATCH2 0x01100000

int ext2_load_file(struct ext2_sb *sb, char *filename, int filename_size, char *rootdir, uint32_t rootdir_size, uint32_t addr) {
	printf("Loading %s at SDRAM_OFFSET(0x%x)... \n", filename, addr);
	uint32_t inum=ext2_inode_num(sb, filename, filename_size, rootdir, rootdir_size); 
	if(inum>0) {
		char* ddest=(char*)(SDRAM_OFFSET(addr));
		uint32_t fsize=ext2_read_inode_contents(sb, inum, 65535, (char*)(SDRAM_OFFSET(LOAD_SCRATCH)), ddest);
		uint32_t nsectors=(fsize+1023)/1024;
		printf("End at SDRAM_OFFSET(0x%x)\n", addr+1024*nsectors);
		return(fsize);
	} else {
		printf("file not found\n");
		return(-1);
	}
}

/* main function */
int load_ext2(phys_addr_t *uboot_base, phys_addr_t *optee_base, \
		phys_addr_t *monitor_base, phys_addr_t *rtos_base, \
		phys_addr_t *opensbi_base, phys_addr_t *dtb_base, char **cmdline) {
	int rc;
	int part_num;
	char *mbr;
	mbr=(char*)SDRAM_OFFSET(LOAD_SCRATCH2);
	char *buf;
	buf=(char*)SDRAM_OFFSET(LOAD_SCRATCH2+1024);
	char *rootdir;
	rootdir=(char*)SDRAM_OFFSET(LOAD_SCRATCH2+2048);
	//printf("addr rootdir=%lx\n",rootdir);
	struct ext2_sb sbb;
	struct ext2_sb *sb=&sbb; 

	//printf("addr &rc=%lx &part_num=%lx mbr=%lx buf=%lx rootdir=%lx",&rc,&part_num,mbr,buf,rootdir);

	*optee_base=*monitor_base=*rtos_base=0;
	*cmdline=NULL;

	if((rc=sunxi_mmc_init(SDC_NO, 4, BT0_head.prvt_head.storage_gpio, 16))<0)
		return(rc);

	/* fetch MBR */
	if((rc=mmc_bread(SDC_NO, 0, 1, mbr))<0) {
		printf("Error reading MBR\n");
		return(rc);
	}
	if(mbr[510]!=0x55 || mbr[511]!=0xAA) {
		printf("Invalid MBR signature\n");
		return(-1);
	}

	/* find an ext2 filesystem marked as bootable in part. table */
	for(part_num=0; part_num<3; part_num++) {
		if(ext2_sb_read(mbr, part_num, buf, sb)>=0) 
			break;
	}
	if(part_num==3) {
		printf("No suitable partition found\n");
		return(-1);
	}

	/* read root directory (inode 2) */
	uint32_t rootdir_size=ext2_read_inode_contents(sb, 2, 1, (char*)(SDRAM_OFFSET(LOAD_SCRATCH)), rootdir); 

/*
	printf("rootdir size=%d\n", rootdir_size);
	for(int i=0;i<rootdir_size;i++) {
		printf("%x ", rootdir[i]);
	}
*/

/*
	uint32_t inum=ext2_inode_num(sb, "hello.bin", 9, rootdir, rootdir_size); 
	if(inum>0) {
		char* ddest=(char*)(SDRAM_OFFSET(0x01000000));
		uint32_t fsize=ext2_read_inode_contents(sb, inum, 20000, (char*)(SDRAM_OFFSET(LOAD_SCRATCH)), ddest);
		uint32_t toffs=15*1048576-4;
		if(fsize);
		printf("read : %x %x %x %x\n", ddest[toffs], ddest[toffs+1], ddest[toffs+2], ddest[toffs+3]);
	} else {
		printf("file not found\n");
	}
*/

#define SBI_OFF 0
#define FDT_OFF 0x4000000
#define IMG_OFF 0x200000  // 0xa000000
//#define       0x1010000

	ext2_load_file(sb, "opensbi.bin", 11, rootdir, rootdir_size, SBI_OFF);
	ext2_load_file(sb, "fdt", 3, rootdir, rootdir_size, FDT_OFF);
	int imgsz=ext2_load_file(sb, "Image", 5, rootdir, rootdir_size, IMG_OFF);
	if(imgsz);
	printf("begin image:\n");
	for(int i=0;i<32;i++) printf("%x ",*(char*)(SDRAM_OFFSET(IMG_OFF+i)));
	printf("end image:\n");
	for(int i=imgsz-32;i<imgsz;i++) printf("%x ",*(char*)(SDRAM_OFFSET(IMG_OFF+i)));


	*uboot_base=SDRAM_OFFSET(IMG_OFF);
	*opensbi_base=SDRAM_OFFSET(SBI_OFF); //SDRAM_OFFSET(IMG_OFF); //SDRAM_OFFSET(SBI_OFF);
	*dtb_base=SDRAM_OFFSET(FDT_OFF);

/*
	volatile char *iob=sunxi_get_iobase(SUNXI_UART0_BASE);
	printf("uart_iobase=%lx\n", iob);

	iob=(volatile char *)0x02500000;
	printf("uart_iobase=%lx\n", iob);
	while(1) *iob='b';
*/

	return(0);
}
