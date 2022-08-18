
#include <common.h>
#include <spare_head.h>
#include <nand_boot0.h>
#include <private_toc.h>
#include <private_boot0.h>
#include <private_uboot.h>
#include <private_tee.h>
#include <private_atf.h>
#include <u-boot/zlib.h>
#include <lzma/LzmaTools.h>
#include <u-boot/lz4.h>
extern const boot0_file_head_t  BT0_head;
#define SDC_NO 0

int ext2_load(phys_addr_t *uboot_base, phys_addr_t *optee_base, \
		phys_addr_t *monitor_base, phys_addr_t *rtos_base, phys_addr_t *opensbi_base, phys_addr_t *dtb_base, char **cmdline) {
	int rc;

	*optee_base=*monitor_base=*rtos_base=0;
	*cmdline=NULL;

	if((rc=sunxi_mmc_init(SDC_NO, 4, BT0_head.storage_gpio, 16))<0)
		return(rc);


}
