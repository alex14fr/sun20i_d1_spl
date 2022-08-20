
#include <common.h>
#include <private_boot0.h>
#include <spare_head.h>
#include <mmc_boot0.h>
extern const boot0_file_head_t  BT0_head;
#define SDC_NO 0

int load_ext2(phys_addr_t *uboot_base, phys_addr_t *optee_base, \
		phys_addr_t *monitor_base, phys_addr_t *rtos_base, phys_addr_t *opensbi_base, phys_addr_t *dtb_base, char **cmdline) {
	int rc;

	*optee_base=*monitor_base=*rtos_base=0;
	*cmdline=NULL;

	if((rc=sunxi_mmc_init(SDC_NO, 4, BT0_head.prvt_head.storage_gpio, 16))<0)
		return(rc);

	printf("mmc_init ok\n");

	char *buf=malloc(512);

	if((rc=mmc_bread(SDC_NO, 0, 1, buf))<0) {
		return(rc);
	}

	printf("read : %x %x %x %x %x\n", buf[0], buf[1], buf[2], buf[3], buf[4]);

	while(1);

	return(0);
}
