/*
 *    COPYRIGHT NOTICE
 *   Copyright 2019 Horizon Robotics, Inc.
 *    All rights reserved.
 */
#include <common.h>
#include <cli.h>
#include <sata.h>
#include <ahci.h>
#include <scsi.h>
#include <malloc.h>
#include <asm/io.h>
#include <cpu.h>
#include <veeprom.h>
#include <sysreset.h>
#include <linux/libfdt.h>
#include <mapmem.h>
#include <part_efi.h>
#include <mmc.h>
#include <part.h>
#include <memalign.h>
#include <ota.h>
#include <spi_flash.h>
#include <asm/gpio.h>
#include <asm/arch/hb_reg.h>
#ifdef CONFIG_HB_BIFSD
#include <asm/arch/hb_bifsd.h>
#endif
#include <asm/arch/hb_sysctrl.h>
#include <asm/arch/hb_pmu.h>
#include <asm/arch/hb_share.h>
#include <configs/hb_config.h>
#include <hb_info.h>
#ifdef CONFIG_HB_NAND_BOOT
#include <ubi_uboot.h>
#endif
#include <asm/arch-x3/hb_reg.h>
#include <asm/arch-x3/boot_mode.h>
#include <asm/arch-x2/ddr.h>
#include <i2c.h>
#include <linux/mtd/mtd.h>
#include <x3_spacc.h>
#include <x3_pka.h>

extern struct spi_flash *flash;
#ifndef CONFIG_FPGA_HOBOT
extern unsigned int sys_sdram_size;
extern bool recovery_sys_enable;
#endif
#if defined(HB_SWINFO_DUMP_OFFSET) && !defined(CONFIG_FPGA_HOBOT)
static int stored_dumptype;
#endif

extern unsigned int hb_gpio_get(void);
extern unsigned int hb_gpio_to_borad_id(unsigned int gpio_id);
uint32_t x3_som_type = SOM_TYPE_X3;

#ifdef CONFIG_SYSRESET
static int print_resetinfo(void)
{
	struct udevice *dev;
	char status[256];
	int ret;

	ret = uclass_first_device_err(UCLASS_SYSRESET, &dev);
	if (ret) {
		debug("%s: No sysreset device found (error: %d)\n",
		      __func__, ret);
		/* Not all boards have sysreset drivers available during early
		 * boot, so don't fail if one can't be found.
		 */
		return 0;
	}

	if (!sysreset_get_status(dev, status, sizeof(status)))
		printf("%s", status);

	return 0;
}
#endif

#if defined(CONFIG_DISPLAY_CPUINFO) && CONFIG_IS_ENABLED(CPU)
static int print_cpuinfo(void)
{
	struct udevice *dev;
	char desc[512];
	int ret;

	ret = uclass_first_device_err(UCLASS_CPU, &dev);
	if (ret) {
		debug("%s: Could not get CPU device (err = %d)\n",
		      __func__, ret);
		return ret;
	}

	ret = cpu_get_desc(dev, desc, sizeof(desc));
	if (ret) {
		debug("%s: Could not get CPU description (err = %d)\n",
		      dev->name, ret);
		return ret;
	}

	printf("%s", desc);

	return 0;
}
#endif

#ifdef HB_AUTOBOOT
static int boot_stage_mark(int stage)
{
        int ret = 0;

        switch (stage) {
        case 0:
                writel(BOOT_STAGE0_VAL, BIF_SHARE_REG_OFF);
                break;
        case 1:
                writel(BOOT_STAGE1_VAL, BIF_SHARE_REG_OFF);
                break;
        case 2:
                writel(BOOT_STAGE2_VAL, BIF_SHARE_REG_OFF);
                break;
        case 3:
                writel(BOOT_STAGE3_VAL, BIF_SHARE_REG_OFF);
                break;
        default :
                ret = -EINVAL;
                break;
        }

        return ret;
}
#endif

static int bif_change_reset2gpio(void)
{
	unsigned int reg_val;

	/*set gpio1[15] GPIO function*/
	reg_val = readl(GPIO1_CFG);
	reg_val |= 0xc0000000;
	writel(reg_val, GPIO1_CFG);
	return 0;
}


#if defined(CONFIG_HB_BIFSD)
static int initr_bifsd(void)
{
	hb_bifsd_initialize();
	return 0;
}
#endif /* CONFIG_HB_BIFSD */
#ifndef CONFIG_FPGA_HOBOT
static int  disable_cnn(void)
{
	u32 reg;
	/* Disable clock of CNN */
	writel(0x33, HB_CNNSYS_CLKEN_CLR);
	while (!((reg = readl(HB_CNNSYS_CLKOFF_STA)) & 0xF));

	udelay(5);

	reg = readl(HB_PMU_VDD_CNN_CTRL) | 0x22;
	writel(reg, HB_PMU_VDD_CNN_CTRL);
	udelay(5);

	writel(0x3, HB_SYSC_CNNSYS_SW_RSTEN);
	udelay(5);

	printf("Disable cnn cores ..\n");
	return 0;
}
#endif

static int bif_recover_reset_func(void)
{
	unsigned int reg_val;

	/*set gpio1[15] GPIO function*/
	reg_val = readl(GPIO1_CFG);
	reg_val &= ~(0xc0000000);
	writel(reg_val, GPIO1_CFG);
	return 0;
}

#define HB_AP_ROOTFS_TYPE_NONE			1
#define HB_AP_ROOTFS_TYPE_CPIO			2
#define HB_AP_ROOTFS_TYPE_SQUASHFS		3
#define HB_AP_ROOTFS_TYPE_CRAMFS		4

static int apbooting(void)
{
	unsigned int kernel_addr, dtb_addr, initrd_addr, rootfstype;
	char cmd[256] = { 0 };
	char *rootfstype_str;
	char *squashfs_str = "squashfs";
	char *cramfs_str = "cramfs";

	bif_change_reset2gpio();
	if (readl(HB_SHARE_BOOT_KERNEL_CTRL) == 0x5aa5) {
		writel(DDRT_UBOOT_RDY_BIT, HB_SHARE_DDRT_CTRL);
		printf("-- wait for kernel\n");
		while (!(readl(HB_SHARE_DDRT_CTRL) == 0)) {}
		kernel_addr = readl(HB_SHARE_KERNEL_ADDR);
		dtb_addr = readl(HB_SHARE_DTB_ADDR);
		rootfstype = readl(HB_SHARE_ROOTFSTYPE_ADDR);
		initrd_addr = readl(HB_SHARE_INITRD_ADDR);
		bif_recover_reset_func();
		// set bootargs if input rootfs is cramfs or squashfs
		if ((HB_AP_ROOTFS_TYPE_SQUASHFS == rootfstype)
				|| (HB_AP_ROOTFS_TYPE_CRAMFS == rootfstype)) {
			if (HB_AP_ROOTFS_TYPE_SQUASHFS == rootfstype)
				rootfstype_str = squashfs_str;
			else
				rootfstype_str = cramfs_str;
			snprintf(cmd, sizeof(cmd)-1, "earlycon console=ttyS0 clk_ignore_unused "
					"root=/dev/ram0 ro initrd=0x%x,100M rootfstype=%s rootwait",
					initrd_addr, rootfstype_str);
			env_set("bootargs", cmd);
		}
		snprintf(cmd, sizeof(cmd)-1, "booti 0x%x - 0x%x", kernel_addr, dtb_addr);
		printf("cmd: %s\n", cmd);
		run_command_list(cmd, -1, 0);
		// run_command_list("booti 0x80000 - 0x10000000", -1, 0);
	}
	bif_recover_reset_func();
	return 0;
}

#ifdef CONFIG_AP_CP_COMN_MODE
struct hb_ap_comn_handle{
	unsigned int magic[4];		/* 0xFEFEFEFE */
	unsigned int cmd_status[4]; /* 0: free 1: ready 2: running 3:finish */
	unsigned int cmd_result[4]; /* 0: success  1: failed  */
};

#define AP_CP_COMN_ADDR (HB_BOOTINFO_ADDR)
#define AP_BUF_ADDR (AP_CP_COMN_ADDR + 0x200)
#define CP_BUF_ADDR (AP_CP_COMN_ADDR + 0x300)
#define STATUS_READ 0
#define STATUS_WRITE 1

static void receive_msg(char *buf, unsigned int size)
{
	if (NULL == buf)
		return;

	memcpy(buf, (void *)AP_BUF_ADDR, size);
}

static void send_msg(char *buf, unsigned int size)
{
	if (NULL == buf)
		return;

	memcpy((void *)CP_BUF_ADDR, buf, size);
}

static void hb_handle_status(struct hb_ap_comn_handle *handle,
	unsigned int flag)
{
	if (NULL == handle)
		return;

	/* 0: read  1: write */
	if (flag == 0)
		memcpy(handle, (void *)AP_CP_COMN_ADDR, sizeof(struct hb_ap_comn_handle));
	else
		memcpy((void *)AP_CP_COMN_ADDR, handle, sizeof(struct hb_ap_comn_handle));
}

static int hb_ap_communication(void)
{
	char receive_info[256] = { 0 };
	char send_info[256] = { 0 };
	int ret = 0;
	char cache_off[32] = "dcache off";
	struct hb_ap_comn_handle handle = { 0 };

	/* magic check */
	hb_handle_status(&handle, STATUS_READ);
	if (handle.magic[0] != 0xFEFEFEFE)
		return 0;

	/* close uboot dcache */
	run_command_list(cache_off, -1, 0);

	/* init handle */
	handle.cmd_status[0] = 0;
	handle.cmd_result[0] = 0;

	/* update status to memory */
	hb_handle_status(&handle, STATUS_WRITE);

	printf("***** uboot AP-CP communication mode ! *****\n");
	printf("***** waiting cmd from AP: *****\n");

	while(1) {
		/* read status */
		hb_handle_status(&handle, STATUS_READ);

		if (handle.cmd_status[0] == 0 || handle.cmd_status[0] == 3) {
			mdelay(1000);
		} else {
			/* read cmd from buffer */
			receive_msg(receive_info, sizeof(receive_info));
			snprintf(receive_info, sizeof(receive_info), "%s", receive_info);

			/* update status */
			handle.cmd_status[0] = 2;
			hb_handle_status(&handle, STATUS_WRITE);

			/* run command */
			printf("receive_cmd : %s\n", receive_info);
			printf("run cmd : \n");
			ret = run_command_list(receive_info, -1, 0);
			if (ret != 0) {
				handle.cmd_result[0] = 1;
				snprintf(send_info, sizeof(send_info), "CP run cmd failed! ");
			} else {
				handle.cmd_result[0] = 0;
				snprintf(send_info, sizeof(send_info), "CP run cmd success! ");
			}

			/* send info */
			send_msg(send_info, sizeof(send_info));

			/* update status */
			handle.cmd_status[0] = 3;
			hb_handle_status(&handle, STATUS_WRITE);

			printf("run cmd finish !\n");
			printf("***** waiting cmd from AP: *****\n");
		}
	}

	return 0;
}
#endif

#ifdef HB_AUTOBOOT
static void wait_start(void)
{
    uint32_t reg_val = 0;
    uint32_t boot_flag = 0xFFFFFFFF;
    uint32_t hb_ip = 0;
    char hb_ip_str[32] = {0};

	/* nfs boot argument */
	uint32_t nfs_server_ip = 0;
	char nfs_server_ip_str[32] = {0};
	char nfs_args[256] = {0};

	/* nc:netconsole argument */
	uint32_t nc_server_ip = 0;
	uint32_t nc_mac_high = 0;
	uint32_t nc_mac_low = 0;
	char nc_server_ip_str[32] = {0};
	char nc_server_mac_str[32] = {0};

	char nc_args[256] = {0};


	char bootargs_str[512] = {0};

	while (1) {
		reg_val = readl(BIF_SHARE_REG_OFF);
		if (reg_val == BOOT_WAIT_VAL)
				break;
		printf("wait for client send start cmd: 0xaa\n");
		mdelay(1000);
	}

	/* nfs boot server argument parse */
	boot_flag = readl(BIF_SHARE_REG(5));
	if ((boot_flag&0xFF) == BOOT_VIA_NFS) {
		hb_ip = readl(BIF_SHARE_REG(6));
		snprintf(hb_ip_str, sizeof(hb_ip_str), "%d.%d.%d.%d",
		(hb_ip>>24)&0xFF, (hb_ip>>16)&0xFF, (hb_ip>>8)&0xFF, (hb_ip)&0xFF);

		nfs_server_ip = readl(BIF_SHARE_REG(7));
		snprintf(nfs_server_ip_str, sizeof(nfs_server_ip_str), "%d.%d.%d.%d",
			(nfs_server_ip>>24)&0xFF, (nfs_server_ip>>16)&0xFF,
			(nfs_server_ip>>8)&0xFF, (nfs_server_ip)&0xFF);

		snprintf(nfs_args, sizeof(nfs_args), "root=/dev/nfs "\
				"nfsroot=%s:/nfs_boot_server,nolock,nfsvers=3 rw "\
				"ip=%s:%s:192.168.1.1:255.255.255.0::eth0:off rdinit=/linuxrc ",
				nfs_server_ip_str, hb_ip_str, nfs_server_ip_str);
	}

	/* netconsole server argument parse */
	if ((boot_flag>>8&0xFF) == NETCONSOLE_CONFIG_VALID) {
		hb_ip = readl(BIF_SHARE_REG(6));
		snprintf(hb_ip_str, sizeof(hb_ip_str), "%d.%d.%d.%d",
		(hb_ip>>24)&0xFF, (hb_ip>>16)&0xFF, (hb_ip>>8)&0xFF, (hb_ip)&0xFF);

		nc_server_ip = readl(BIF_SHARE_REG(9));
		snprintf(nc_server_ip_str, sizeof(nc_server_ip_str), "%d.%d.%d.%d",
			(nc_server_ip>>24)&0xFF, (nc_server_ip>>16)&0xFF,
			(nc_server_ip>>8)&0xFF, (nc_server_ip)&0xFF);

		nc_mac_high = readl(BIF_SHARE_REG(10));
		nc_mac_low = readl(BIF_SHARE_REG(11));
		snprintf(nc_server_mac_str, sizeof(nc_server_mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
			(nc_mac_high>>8)&0xFF, (nc_mac_high)&0xFF,
			(nc_mac_low>>24)&0xFF, (nc_mac_low>>16)&0xFF,
			(nc_mac_low>>8)&0xFF, (nc_mac_low)&0xFF);

		snprintf(nc_args, sizeof(nc_args), "netconsole=6666@%s/eth0,9353@%s/%s ",
			hb_ip_str, nc_server_ip_str, nc_server_mac_str);
	}

	snprintf(bootargs_str, sizeof(bootargs_str), "earlycon clk_ignore_unused %s %s", nfs_args, nc_args);
	env_set("bootargs", bootargs_str);
	return;
}
#endif

uint32_t hb_base_board_type_get(void)
{
	uint32_t reg, base_id;

	reg = reg32_read(X2_GPIO_BASE + X3_GPIO0_VALUE_REG);
	base_id = PIN_BASE_BOARD_SEL(reg) + 1;

	return base_id;
}

static bool hb_pf5024_device_id_getable(void)
{
	uint8_t chip = I2C_PF5024_SLAVE_ADDR;
	uint32_t addr = 0;
	uint8_t device_id = 0;
	int ret;
	struct udevice *dev;

	ret = i2c_get_cur_bus_chip(chip, &dev);
	if (!ret)
		ret = i2c_set_chip_offset_len(dev, 1);
	else
		return false;

	ret = dm_i2c_read(dev, addr, &device_id, 1);
	if (ret)
		return false;

	if (device_id == 0x54)
		return true;
	else
		return false;
}

uint32_t hb_board_type_get(void)
{
	uint32_t board_type, base_board_id, som_id;
	bool flag = hb_pf5024_device_id_getable();

	base_board_id = hb_base_board_type_get();

	if (flag)
		som_id = SOM_TYPE_J3;
	else
		som_id = SOM_TYPE_X3;

	x3_som_type = som_id;

	board_type = (base_board_id & 0x7) | ((som_id & 0x7) << 8);
	printf("board_type = %02x\n", board_type);

	return board_type;
}

#ifdef CONFIG_HB_NAND_BOOT
static void nand_boot(void)
{
	run_command("mtd list", 0);
	if (ubi_part("sys", NULL)) {
		printf("system ubi image load failed!\n");
	}
}

static void nand_dtb_image_load(unsigned int addr, unsigned int leng,
	bool flag)
{
	char cmd[256] = { 0 };
	ulong gz_addr = env_get_ulong("gz_addr", 16, GZ_ADDR);
	ulong dtb_addr = env_get_ulong("fdt_addr", 16, FDT_ADDR);

	if (flag == false) {
		ubi_volume_read("dtb", (void *)dtb_addr, leng);
	} else {
		ubi_volume_read("dtb", (void *)gz_addr, 0);

		snprintf(cmd, sizeof(cmd), "unzip 0x%lx 0x%lx", gz_addr, dtb_addr);
		run_command_list(cmd, -1, 0);
		gz_addr = dtb_addr + addr;
		if (gz_addr != dtb_addr)
			memcpy((void *)dtb_addr, (void *)gz_addr, NOR_SECTOR_SIZE);
	}
}

static void hb_nand_env_init(void)
{
	char *tmp;

	/* set bootargs */
	tmp = "earlycon loglevel=8 console=ttyS0 clk_ignore_unused "\
		"root=ubi0:rootfs ubi.mtd=2,2048 rootfstype=ubifs rw rootwait";
	env_set("bootargs", tmp);

	/* set bootcmd */
	tmp = "send_id;run unzipimage;ion_modify ${ion_size};"\
		"mem_modify ${mem_size};run ddrboot;";
	env_set("bootcmd", tmp);
}

static void hb_nand_dtb_load(unsigned int board_id,
		struct hb_kernel_hdr *config)
{
	char *s = NULL;
	bool gz_flag = false;
	int i, count = config->dtb_number;
	uint32_t board_type = hb_board_type_get();

	if (count > DTB_MAX_NUM) {
		printf("error: count %02x not support\n", count);
		return;
	}

	if (config->dtb[0].dtb_addr == 0)
		gz_flag = true;

	for (i = 0; i < count; i++) {
		if (board_type == config->dtb[i].board_id) {
			s = (char *)config->dtb[i].dtb_name;

			if ((config->dtb[i].dtb_addr == 0x0) && (gz_flag == false)) {
				printf("error: dtb %s not exit\n", s);
				return;
			}

			nand_dtb_image_load(config->dtb[i].dtb_addr,
				config->dtb[i].dtb_size, gz_flag);

			hb_nand_env_init();
			break;
		}
	}

	if (i == count)
		printf("error: board_id %02x not support\n", board_id);

	printf("fdtimage = %s\n", s);
	env_set("fdtimage", s);
}

static void hb_nand_kernel_load(struct hb_kernel_hdr *config)
{
	ulong gz_addr;
	bool load_imggz = true;
	int ret;
	gz_addr = env_get_ulong("gz_addr", 16, GZ_ADDR);
	ret = 0;

	/* load Image.gz */
	ret = ubi_volume_read("kernel", (void *)gz_addr, 0);

	if (ret) {
		printf("Load from ubi volume failed with error: -%d!\n", ret);
	}
}
#endif

#ifndef CONFIG_FPGA_HOBOT
static void hb_mmc_env_init(void)
{
	char upmode[16] = { 0 };
	char boot_reason[64] = { 0 };
	char *s;
	char *verify = "avb_verify;";
	char count;
	char cmd[256] = { 0 };

	veeprom_read(VEEPROM_RESET_REASON_OFFSET, boot_reason,
		VEEPROM_RESET_REASON_SIZE);

	veeprom_read(VEEPROM_UPDATE_MODE_OFFSET, upmode,
			VEEPROM_UPDATE_MODE_SIZE);

	if ((strcmp(upmode, "AB") == 0) || (strcmp(upmode, "golden") == 0)) {
		if (strcmp(boot_reason, "normal") == 0) {
			/* boot_reason is 'normal', normal boot */
			printf("uboot: normal boot \n");

			veeprom_read(VEEPROM_COUNT_OFFSET, &count,
				VEEPROM_COUNT_SIZE);

			if (count <= 0) {
				if (strcmp(upmode, "AB") == 0) {
					/* AB mode, boot system backup partition */
					ota_ab_boot_bak_partition();
				} else {
					/* golden mode, boot recovery system */
					ota_recovery_mode_set(true);
				}
			} else {
				ota_upgrade_flag_check(upmode, boot_reason);
			}
		} else if (strcmp(boot_reason, "recovery") == 0) {
			/* boot_reason is 'recovery', enter recovery mode */
			env_set("bootdelay", "0");
			ota_recovery_mode_set(false);
		} else {
			ota_upgrade_flag_check(upmode, boot_reason);

			/* auto extend last emmc partition */
			if (strcmp(boot_reason, "all") == 0) {
				s = "gpt extend mmc 0";
				run_command_list(s, -1, 0);
			}
		}
	}

	/* init env mem_size */
	s = env_get("mem_size");
	if (s == NULL) {
		// uint32_to_char(sys_sdram_size, cmd);
		snprintf(cmd, sizeof(cmd), "%02x", sys_sdram_size);
		env_set("mem_size", cmd);
	}

	if(strcmp(boot_partition, "recovery") == 0)
		verify = "";

	/* init env bootcmd init */
	snprintf(cmd, sizeof(cmd), "%s part size mmc 0 %s " \
		"bootimagesize;part start mmc 0 %s bootimageblk;"\
		"mmc read 0x10000000 ${bootimageblk} ${bootimagesize};" \
		"bootm 0x10000000;", verify, boot_partition, boot_partition);
	env_set("bootcmd", cmd);
}
#endif

static int hb_nor_dtb_handle(struct hb_kernel_hdr *config)
{
	char *s = NULL;
	void *dtb_addr, *base_addr = (void *)config;
	uint32_t i = 0;
	uint32_t count = config->dtb_number;
	uint32_t board_type = hb_board_type_get();

	if (count > DTB_MAX_NUM) {
		printf("error: count %02x not support\n", count);
		return -1;
	}

	for (i = 0; i < count; i++) {
		if (board_type == config->dtb[i].board_id) {
			s = (char *)config->dtb[i].dtb_name;
			break;
		}
	}

	if (i == count) {
		printf("error: base_board_id %02x not support\n", board_type);
		return -1;
	}

	printf("fdtimage = %s\n", s);
	env_set("fdtimage", s);

	/* config dtb image */
	dtb_addr = base_addr + sizeof(struct hb_kernel_hdr) + \
		config->dtb[i].dtb_addr;
	memcpy(base_addr, dtb_addr, config->dtb[i].dtb_size);

	return 0;
}

#if 0
static bool hb_nor_ota_upflag_check(void)
{
	char upmode[16] = { 0 };
	char boot_reason[64] = { 0 };
	bool load_imggz = true;
	char count;

	veeprom_read(VEEPROM_RESET_REASON_OFFSET, boot_reason,
		VEEPROM_RESET_REASON_SIZE);

	veeprom_read(VEEPROM_UPDATE_MODE_OFFSET, upmode,
			VEEPROM_UPDATE_MODE_SIZE);

	if ((strcmp(upmode, "golden") == 0)) {
		if (strcmp(boot_reason, "normal") == 0) {
			/* boot_reason is 'normal', normal boot */
			printf("uboot: normal boot \n");

			veeprom_read(VEEPROM_COUNT_OFFSET, &count,
					VEEPROM_COUNT_SIZE);
			if (count <= 0) {
				/* system boot failed, enter recovery mode */
				ota_recovery_mode_set(true);

				if (recovery_sys_enable)
					load_imggz = false;
			}
		} else if (strcmp(boot_reason, "recovery") == 0) {
			/* boot_reason is 'recovery', enter recovery mode */
			env_set("bootdelay", "0");

			load_imggz = false;
		} else {
			/* check update_success flag */
			if (ota_check_update_success_flag()) {
				load_imggz = false;
			} else {
				/* ota partition status check */
				ota_upgrade_flag_check(upmode, boot_reason);
			}
		}
	}
	return load_imggz;
}
#endif

static void hb_nor_boot(void)
{
	char *bootargs;
	char *bootcmd;
	char *boot_arg[2];
	loff_t offset, size, maxsize;
//	bool load_imggz = true;
	int dev = 0;

	if (flash == NULL) {
		printf("Error: flash init failed\n");
		return;
	}

//	load_imggz = hb_nor_ota_upflag_check();
	boot_arg[1] = "0x0";
/* 	if (load_imggz) {
		boot_arg[0] = "boot";
	} else {
		boot_arg[0] = "recovery";
	} */
	boot_arg[0] = "boot";

	/* set normal boot bootargs */
	bootargs = "earlycon loglevel=8 console=ttyS0 clk_ignore_unused "\
			 "root=ubi0:rootfs ubi.mtd=0 rootfstype=ubifs rw rootwait";
	env_set("bootargs", bootargs);
	/* set secure boot bootcmd */
	if (hb_check_secure()) {
		if (run_command("avb_verify", 0))
			return;
	}
	if (mtd_arg_off_size(2, boot_arg, &dev, &offset, &maxsize,
						&size, 0x0001, flash->size)) {
		return;
	}
	spi_flash_read(flash, offset, size, (void *) 0x10000000);
	bootcmd = "bootm 0x10000000;";

	env_set("bootcmd", bootcmd);
}

static void hb_usb_dtb_config(void) {
	ulong dtb_addr = env_get_ulong("fdt_addr", 16, FDT_ADDR);
	struct hb_kernel_hdr *head = (struct hb_kernel_hdr *)dtb_addr;

	hb_nor_dtb_handle(head);
}

static void hb_usb_env_init(void)
{
	char *tmp = "send_id;run ddrboot";

	/* set bootcmd */
	env_set("bootcmd", tmp);
}

#ifndef CONFIG_FPGA_HOBOT
static void hb_env_and_boardid_init(void)
{
	char *s = NULL;
	char boot_reason[64] = { 0 };
	unsigned int boot_mode = hb_boot_mode_get();

	printf("bootinfo/board_id = %02x\n", x3_board_id);

	/* init env recovery_sys_enable */
	s = env_get("recovery_system");
	if (strcmp(s, "disable") == 0) {
		recovery_sys_enable = false;

		/* config resetreason */
		veeprom_read(VEEPROM_RESET_REASON_OFFSET, boot_reason,
			VEEPROM_RESET_REASON_SIZE);
		if (strcmp(boot_reason, "recovery") == 0)
			veeprom_write(VEEPROM_RESET_REASON_OFFSET, "normal",
			VEEPROM_RESET_REASON_SIZE);
	}

	/* mmc or nor env init */
	if (boot_mode == PIN_2ND_EMMC) {
		hb_mmc_env_init();
	}  else if (boot_mode == PIN_2ND_NOR) {
		/* load nor kernel and dtb */
		hb_nor_boot();
	}

#ifdef CONFIG_HB_NAND_BOOT
	struct hb_kernel_hdr *hb_kernel_conf;
	ubi_volume_read("dtb_mapping", (void *)HB_DTB_CONFIG_ADDR, 0);
	/* load nand kernel and dtb */
	hb_kernel_conf =  (struct hb_kernel_hdr *)HB_DTB_CONFIG_ADDR;
	hb_nand_dtb_load(x3_board_id, hb_kernel_conf);
	hb_nand_kernel_load(hb_kernel_conf);
#endif
}

static int fdt_get_reg(const void *fdt, void *buf, u64 *address, u64 *size)
{
	int address_cells = fdt_address_cells(fdt, 0);
	int size_cells = fdt_size_cells(fdt, 0);
	char *p = buf;

	if (address_cells == 2)
		*address = fdt64_to_cpu(*(fdt64_t *)p);
	else
		*address = fdt32_to_cpu(*(fdt32_t *)p);
	p += 4 * address_cells;

	if (size_cells == 2)
		*size = fdt64_to_cpu(*(fdt64_t *)p);
	else
		*size = fdt32_to_cpu(*(fdt32_t *)p);
	p += 4 * size_cells;

	return 0;
}

static int fdt_pack_reg(const void *fdt, void *buf, u64 address, u64 size)
{
	int address_cells = fdt_address_cells(fdt, 0);
	int size_cells = fdt_size_cells(fdt, 0);
	char *p = buf;

	if (address_cells == 2)
		*(fdt64_t *)p = cpu_to_fdt64(address);
	else
		*(fdt32_t *)p = cpu_to_fdt32(address);
	p += 4 * address_cells;

	if (size_cells == 2)
		*(fdt64_t *)p = cpu_to_fdt64(size);
	else
		*(fdt32_t *)p = cpu_to_fdt32(size);
	p += 4 * size_cells;

	return p - (char *)buf;
}

static int do_change_ion_size(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	const char *path;
	int  nodeoffset;
	char *prop;
	static char data[1024] __aligned(4);
	void *ptmp;
	int  len;
	void *fdt;
	phys_addr_t fdt_paddr;
	u64 ion_start, old_size;
	u32 size;
	char *s = NULL;
	int ret;

	if (argc > 1)
		s = argv[1];

	if (s) {
		size = (u32)simple_strtoul(s, NULL, 10);
		if (size == 0)
			return 0;

		if (size < 64)
			size = 64;
	} else {
		return 0;
	}

	s = env_get("fdt_addr");
	if (s) {
		fdt_paddr = (phys_addr_t)simple_strtoull(s, NULL, 16);
		fdt = map_sysmem(fdt_paddr, 0);
	} else {
		printf("Can't get fdt_addr !!!");
		return 0;
	}

	path = "/reserved-memory/ion_reserved";
	prop = "reg";

	nodeoffset = fdt_path_offset (fdt, path);
	if (nodeoffset < 0) {
		/*
			* Not found or something else bad happened.
			*/
		printf ("libfdt fdt_path_offset() returned %s\n",
			fdt_strerror(nodeoffset));
		return 1;
	}
	ptmp = (char *)fdt_getprop(fdt, nodeoffset, prop, &len);
	if (len > 1024) {
		printf("prop (%d) doesn't fit in scratchpad!\n",
				len);
		return 1;
	}
	if (!ptmp)
		return 0;

	fdt_get_reg(fdt, ptmp, &ion_start, &old_size);
	printf("Orign(MAX) Ion Reserve Mem Size to %lldM\n", old_size / 0x100000);

	if (size > old_size / 0x100000) {
		return 0;
	}

	len = fdt_pack_reg(fdt, data, ion_start, size * 0x100000);

	ret = fdt_setprop(fdt, nodeoffset, prop, data, len);
	if (ret < 0) {
		printf ("libfdt fdt_setprop(): %s\n", fdt_strerror(ret));
		return 1;
	}

	printf("Change Ion Reserve Mem Size to %dM\n", size);

	return 0;
}

U_BOOT_CMD(
	ion_modify,	2,	0,	do_change_ion_size,
	"Change ION Reserved Mem Size(Mbyte)",
	"-ion_modify 100"
);

static int do_set_tag_memsize(cmd_tbl_t *cmdtp, int flag,
	int argc, char * const argv[])
{
	u32 size;
	char *s = NULL;

	if (argc > 1)
		s = argv[1];

	if (s) {
		size = (u32)simple_strtoul(s, NULL, 16);
		if ((size == 0) || (size == sys_sdram_size))
			return 0;
	} else {
		return 0;
	}

	printf("uboot mem_size = %02x\n", size);
	printf("gd mem size is %02llx\n", gd->bd->bi_dram[0].size);

	/* set mem size */
	gd->bd->bi_dram[0].size = size;

	return 0;
}

U_BOOT_CMD(
	mem_modify,	2,	0,	do_set_tag_memsize,
	"Change DDR Mem Size(Mbyte)",
	"-mem_modify 0x40000000"
);
#endif
#ifdef HB_AUTORESET
static void prepare_autoreset(void)
{
	printf("prepare for auto reset test ...\n");
	mdelay(50);

	env_set("bootcmd_bak", env_get("bootcmd"));
	env_set("bootcmd", "reset");
	return;
}
#endif

#if 0
//START4[prj_j2quad]
//#if defined(CONFIG_X2_QUAD_BOARD)

#define SWITCH_PORT_RESET_GPIO	22

/*sja1105 needs reset the certain net port when link state change,
or the network won't work properly*/
static int reboot_notify_to_mcu(void)
{
	int ret;
	unsigned int reg_val;

	/*set gpio1[7] GPIO function*/
	reg_val = readl(GPIO1_CFG);
	reg_val |= 0xc000;
	writel(reg_val, GPIO1_CFG);

	ret = gpio_request(SWITCH_PORT_RESET_GPIO, "switch_port_rst_gpio");
	if (ret && ret != -EBUSY) {
		printf("%s: requesting pin %u failed\n", __func__, SWITCH_PORT_RESET_GPIO);
		return -1;
	}

	ret |= gpio_direction_output(SWITCH_PORT_RESET_GPIO, 1);
	udelay(2000);
	ret |= gpio_set_value(SWITCH_PORT_RESET_GPIO, 0);
	mdelay(10);
	ret |= gpio_set_value(SWITCH_PORT_RESET_GPIO, 1);

	gpio_free(SWITCH_PORT_RESET_GPIO);

	return ret;
}
//#endif
//END4[prj_j2quad]
#endif

static int nand_write_partition(char *partition, int partition_offset,
				int partition_size)
{
	int ret = 0;
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "mtd erase %s", partition);
	ret = run_command(cmd, 0);
	if (ret)
		printf("mtd erase partition %s failed\n", partition);
	snprintf(cmd, sizeof(cmd), "mtd write %s %x 0x0 %x", partition,
				partition_offset, partition_size);
	printf("%s\n", cmd);
	ret = run_command(cmd, 0);
	if (ret)
		printf("mtd write partition %s failed\n", partition);
	return ret;
}

static int burn_nand_flash(cmd_tbl_t *cmdtp, int flag,
	int argc, char * const argv[])
{
#define MAX_MTD_PART_NUM 16
#define MAX_MTD_PART_NAME 128
	int ret, last_part, total_part = 0, len = 0, name_len = 0;
	u32 img_addr, img_size;
	/*[0] - bl_size; [1] - boot_size; [2] - rootfs_size; [3] - userdata_size*/
	u32 part_sizes[MAX_MTD_PART_NUM] = { 0 };
	u32 offsets[MAX_MTD_PART_NUM] = { 0 };
	char part_name[MAX_MTD_PART_NAME][MAX_MTD_PART_NUM] = { "" };
	char *s1 = NULL;
	char *s2 = NULL;
	char *mtdparts_env = NULL;
	char *name_start = NULL;
	char *name_end = NULL;
	char curSize[16] = { 0 };
	if (argc < 3) {
		printf("image_addr and img_size must be given\n");
		return 1;
	}
#ifndef CONFIG_HB_NAND_BOOT
	run_command("mtd list", 0);
#endif
	s1 = argv[1];
	s2 = argv[2];
	img_addr = (u32)simple_strtoul(s1, NULL, 16);
	img_size = (u32)simple_strtoul(s2, NULL, 16);
	printf("Reading image of size 0x%x from address: 0x%x\n", img_size, img_addr);
	mtdparts_env = env_get("mtdparts");
	mtdparts_env = strstr(mtdparts_env, "@");
	if (!mtdparts_env) {
		printf("No MTD Partitions found, Abort!\n");
		return 1;
	}
	do {
		mtdparts_env++;
		name_start = strstr(mtdparts_env, "(");
		name_end = strstr(mtdparts_env, ")");
		len = name_start - mtdparts_env;
		name_start++;
		name_len = name_end - name_start;
		strncpy(curSize, mtdparts_env, len);
		strncpy(part_name[total_part], name_start, name_len);
		offsets[total_part] = (u32)simple_strtoul(curSize, NULL, 16);
		part_sizes[total_part - 1] = offsets[total_part] - offsets[total_part - 1];
		total_part++;
		mtdparts_env = strstr(mtdparts_env, "@");
	} while ((mtdparts_env != NULL) && (total_part < MAX_MTD_PART_NUM));
	last_part = total_part - 1;
	part_sizes[last_part] = img_size;
	for (int i = 0; i < total_part - 1; i++) {
		part_sizes[last_part] -= part_sizes[i];
	}
	ret = 0;
#ifdef CONFIG_HB_NAND_BOOT
	char veeprom[2048] = { 0 };
	ubi_part("sys", 0);
	ubi_volume_read("veeprom", veeprom, 2048);
	run_command("ubi detach", 0);
#endif
	for (int i = 0; i < total_part; i++) {
		ret = nand_write_partition(part_name[i],
								   img_addr + offsets[i],
								   part_sizes[i]);
	}
#ifdef CONFIG_HB_NAND_BOOT
	ubi_part("sys", 0);
	ubi_volume_write("veeprom", veeprom, 2048);
#endif
	return ret;
}

U_BOOT_CMD(
	burn_nand,	6,	0,	burn_nand_flash,
	"Burn Image at [addr] in DDR with [size] in bytes(hex) to nand flash",
	"      [addr] [size] \n"
	"      Note: partitions need to be defined by mtdparts"
);


#ifndef	CONFIG_FPGA_HOBOT
#if defined(HB_SWINFO_BOOT_OFFSET)
static int hb_swinfo_boot_uboot_check(void)
{
	uint32_t s_magic, s_boot, s_f = 0;
	void *s_addr;

	s_magic = readl((void*)HB_SWINFO_MEM_ADDR);
	if (s_magic == HB_SWINFO_MEM_MAGIC) {
		s_addr = (void *)(HB_SWINFO_MEM_ADDR + HB_SWINFO_BOOT_OFFSET);
		s_f = 1;
	} else {
		s_addr = (void *)(HB_PMU_SW_REG_00 + HB_SWINFO_BOOT_OFFSET);
	}
	s_boot = readl(s_addr) & 0xF;
	if (s_boot == HB_SWINFO_BOOT_UBOOTONCE ||
		s_boot == HB_SWINFO_BOOT_UBOOTWAIT) {
		puts("wait for swinfo boot: ");
		if (s_boot == HB_SWINFO_BOOT_UBOOTONCE) {
			puts("ubootonce\n");
			writel(s_boot & (~0xF), s_addr);
			if (s_f)
				flush_dcache_all();
		} else {
			puts("ubootwait\n");
		}
		return 1;
	}

	return 0;
}
#endif

#if defined(HB_SWINFO_DUMP_OFFSET)
static int hb_swinfo_dump_check(void)
{
	uint32_t s_magic, s_boot, s_dump;
	void *s_addrb, *s_addrd;
	uint8_t d_ip[5];
	char dump[128];
	char *s = dump;
	const char *dcmd, *ddev;
	uint32_t dmmc, dpart, dusb;

	s_magic = readl((void *)HB_SWINFO_MEM_ADDR);
	if (s_magic == HB_SWINFO_MEM_MAGIC) {
		s_addrb = (void *)(HB_SWINFO_MEM_ADDR + HB_SWINFO_BOOT_OFFSET);
		s_addrd = (void *)(HB_SWINFO_MEM_ADDR + HB_SWINFO_DUMP_OFFSET);
	} else {
		s_addrb = (void *)(HB_PMU_SW_REG_00 + HB_SWINFO_BOOT_OFFSET);
		s_addrd = (void *)(HB_PMU_SW_REG_00 + HB_SWINFO_DUMP_OFFSET);
	}
	s_boot = readl(s_addrb);
	s_dump = readl(s_addrd);
	if (s_boot == HB_SWINFO_BOOT_UDUMPTF ||
		s_boot == HB_SWINFO_BOOT_UDUMPEMMC) {
		stored_dumptype = 1;
		if (s_boot == HB_SWINFO_BOOT_UDUMPTF) {
			ddev = "tfcard";
			dcmd = "fatwrite";
			dmmc = 1;
			dpart = 1;
		} else {
			ddev = "emmc";
			dcmd = "ext4write";
			dmmc = 0;
			dpart = get_partition_id("userdata");
		}
		printf("swinfo dump ddr 0x%x -> %s:%d\n", sys_sdram_size, ddev, dpart);
		s += sprintf(s, "mmc rescan; ");
		s += sprintf(s, "%s mmc %d:%d 0x0 /dump_ddr_%x.img 0x%x",
				dcmd, dmmc, dpart, sys_sdram_size, sys_sdram_size);

		env_set("dumpcmd", dump);
	} else if (s_boot == HB_SWINFO_BOOT_UDUMPUSB) {
		stored_dumptype = 1;
		ddev = "usb";
		dcmd = "fatwrite";
		dusb = 0;

		printf("swinfo dump ddr 0x%x -> %s\n", sys_sdram_size, ddev);
		s += sprintf(s, "usb start; ");
		s += sprintf(s, "usb part %d; ", dusb);
		s += sprintf(s, "%s usb %d 0x0 dump_ddr_%x.img 0x%x; ",
				dcmd, dusb, sys_sdram_size, sys_sdram_size);

		env_set("dumpcmd", dump);
	} else if (s_dump) {
		stored_dumptype = 2;
		d_ip[0] = (s_dump >> 24) & 0xff;
		d_ip[1] = (s_dump >> 16) & 0xff;
		d_ip[2] = (s_dump >> 8) & 0xff;
		d_ip[3] = s_dump & 0xff;
		d_ip[4] = (d_ip[3] == 1) ? 10 : 1;
		printf("swinfo dump ddr 0x%x %d.%d.%d.%d -> %d\n",
			   sys_sdram_size, d_ip[0], d_ip[1], d_ip[2],
			   d_ip[4], d_ip[3]);
		s += sprintf(s, "setenv ipaddr %d.%d.%d.%d;",
					 d_ip[0], d_ip[1], d_ip[2], d_ip[4]);
		s += sprintf(s, "setenv serverip %d.%d.%d.%d;",
					 d_ip[0], d_ip[1], d_ip[2], d_ip[3]);
		s += sprintf(s, "tput 0x0 0x%x dump_ddr_%x.img",
					 sys_sdram_size, sys_sdram_size);
		env_set("dumpcmd", dump);
	} else {
		stored_dumptype = 0;
	}

	return stored_dumptype;
}

static int hb_swinfo_dump_donecheck(int retc)
{
	uint32_t s_magic, s_boot, s_f = 0;
	void *s_addrb, *s_addrd;

	if (retc) {
		printf("swinfo dump ddr error %d.\n", retc);
		return retc;
	}

	s_magic = readl((void *)HB_SWINFO_MEM_ADDR);
	if (s_magic == HB_SWINFO_MEM_MAGIC) {
		s_addrb = (void *)(HB_SWINFO_MEM_ADDR + HB_SWINFO_BOOT_OFFSET);
		s_addrd = (void *)(HB_SWINFO_MEM_ADDR + HB_SWINFO_DUMP_OFFSET);
		s_f = 1;
	} else {
		s_addrb = (void *)(HB_PMU_SW_REG_00 + HB_SWINFO_BOOT_OFFSET);
		s_addrd = (void *)(HB_PMU_SW_REG_00 + HB_SWINFO_DUMP_OFFSET);
	}
	if (stored_dumptype == 1) {
		s_boot = readl(s_addrb);
		writel(s_boot & (~0xF), s_addrb);
	} else if (stored_dumptype == 2) {
		writel(0x00, s_addrd);
	} else {
		s_f = 0;
	}
	if (s_f)
		flush_dcache_all();

	printf("swinfo dump ddr done.\n");
	return 0;
}
#endif
#endif

#if 0
static void misc()
{
	//START4[prj_j2quad]
//#if defined(CONFIG_X2_QUAD_BOARD)
	if (hb_get_boardid()==QUAD_BOARD_ID){
#if defined(CONFIG_J2_LED)
		j2_led_init();
#endif
		reboot_notify_to_mcu();
		printf("=>j2quad<=");
	}
	if ((hb_get_boardid() == J2_MM_BOARD_ID) ||
			(hb_get_boardid() == J2_MM_S202_BOARD_ID)) {
		env_set("kernel_addr", "0x400000");
	}
//#endif
//END4[prj_j2quad]
}
#endif
#ifndef CONFIG_FPGA_HOBOT
static void hb_swinfo_boot(void)
{
	int retc;
	const char *s = NULL;
	int stored_bootdelay = -1;
#if defined(HB_SWINFO_DUMP_OFFSET)
	if(hb_swinfo_dump_check()) {
		s = env_get("dumpcmd");
		stored_bootdelay = 0;
		if (cli_process_fdt(&s))
			cli_secure_boot_cmd(s);
	}
#endif

#if defined(HB_SWINFO_BOOT_OFFSET)
	if(hb_swinfo_boot_uboot_check()) {
		env_set("ubootwait", "wait");
		return;
	}
#endif
	if (stored_bootdelay == 0 && s) {
		retc = run_command_list(s, -1, 0);
#if defined(HB_SWINFO_DUMP_OFFSET)
		if(stored_dumptype)
			hb_swinfo_dump_donecheck(retc);
#endif
		return;
	}
}
#endif

#if defined(CONFIG_FASTBOOT) || defined(CONFIG_USB_FUNCTION_MASS_STORAGE)
int setup_boot_action(int boot_mode)
{
	void *reg = (void *)HB_PMU_SW_REG_04;
	int boot_action = readl(reg);

	if (boot_mode != PIN_2ND_USB && hb_fastboot_key_pressed()) {
		printf("enter fastboot mode(reuse bootsel[15]: 1)\n");
		boot_action = BOOT_FASTBOOT;
	}

	debug("%s: boot action 0x%08x\n", __func__, boot_action);

	/* Clear boot mode */
	writel(BOOT_NORMAL, reg);

	switch (boot_action) {
	case BOOT_FASTBOOT:
		printf("%s: enter fastboot!\n", __func__);
		env_set("preboot", "setenv preboot; fastboot usb 0");
		break;
	case BOOT_UMS:
		printf("%s: enter UMS!\n", __func__);
		env_set("preboot", "setenv preboot; ums mmc 0");
		break;
	}

	return 0;
}
#endif

int board_early_init_r(void)
{
#ifdef HB_AUTOBOOT
        boot_stage_mark(1);
#endif
#ifdef CONFIG_HB_BIFSD
	initr_bifsd();
#endif
	return 0;
}

int board_early_init_f(void)
{
	bif_change_reset2gpio();
	writel(0xFED10000, BIF_SHARE_REG_BASE);
#ifdef HB_AUTOBOOT
        boot_stage_mark(0);
#endif
#if defined(CONFIG_SYSRESET)
	print_resetinfo();
#endif
	return 0;
}

static void base_board_gpio_test(void)
{
	uint32_t  base_board_id = hb_base_board_type_get();

	switch (base_board_id) {
	case BASE_BOARD_X3_DVB:
		printf("base board type: X3 DVB\n");
		break;
	case BASE_BOARD_J3_DVB:
		printf("base board type: J3 DVB\n");
		break;
	case BASE_BOARD_CVB:
		printf("base board type: CVB\n");
		break;
	case BASE_BOARD_CUSTOMER_BOARD:
		printf("base board type: customer board\n");
		break;
	default:
		printf("base board type not support\n");
		break;
	}

}

static void boot_src_test(void)
{
	uint32_t boot_src = hb_boot_mode_get();

	switch (boot_src) {
	case PIN_2ND_EMMC:
		printf("bootmode: EMMC\n");
		break;
	case PIN_2ND_NAND:
		printf("bootmode: NAND\n");
		break;
	case PIN_2ND_AP:
		printf("bootmode: AP\n");
		break;
	case PIN_2ND_USB:
	case PIN_2ND_USB_BURN:
		printf("bootmode: USB\n");
		break;
	case PIN_2ND_NOR:
		printf("bootmode: NOR\n");
		break;
	case PIN_2ND_UART:
		printf("bootmode: UART\n");
		break;
	default:
		printf("bootmode not support\n");
		break;
}
}

int last_stage_init(void)
{
	int boot_mode = hb_boot_mode_get();
#ifndef CONFIG_FPGA_HOBOT
	disable_cnn();
#endif
#ifdef CONFIG_MMC
	veeprom_init();
#endif
	bif_recover_reset_func();
	apbooting();
#ifdef	CONFIG_AP_CP_COMN_MODE
	hb_ap_communication();
#endif

#ifdef CONFIG_HB_NAND_BOOT
	nand_boot();
#endif

	base_board_gpio_test();
	boot_src_test();

	/* spacc and pka init */
	spacc_init();
	pka_init();

#ifdef HB_AUTOBOOT
	boot_stage_mark(2);
	wait_start();
#endif

#ifndef	CONFIG_FPGA_HOBOT
#ifndef CONFIG_DOWNLOAD_MODE
	if ((boot_mode == PIN_2ND_NOR) || (boot_mode == PIN_2ND_NAND) \
		|| (boot_mode == PIN_2ND_EMMC))
		hb_env_and_boardid_init();

	if (boot_mode == PIN_2ND_USB) {
		hb_usb_dtb_config();

		hb_usb_env_init();
	}
#endif
#endif

#ifdef HB_AUTORESET
	prepare_autoreset();
#endif

#ifndef	CONFIG_FPGA_HOBOT
	hb_swinfo_boot();
#endif

#if defined(CONFIG_FASTBOOT) || defined(CONFIG_USB_FUNCTION_MASS_STORAGE)
	setup_boot_action(boot_mode);
#endif

//	misc();

	return 0;
}
