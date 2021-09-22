// SPDX-License-Identifier: GPL-2.0+
/*
 * Command for accessing SPI flash.
 *
 * Copyright (C) 2008 Atmel Corporation
 */

#include <common.h>
#include <command.h>
#include <div64.h>
#include <dm.h>
#include <flash.h>
#include <log.h>
#include <malloc.h>
#include <mapmem.h>
#include <spi.h>
#include <spi_flash.h>
#include <asm/cache.h>
#include <jffs2/jffs2.h>
#include <linux/mtd/mtd.h>
#include <hb_info.h>

#include <asm/io.h>
#include <dm/device-internal.h>
#include <veeprom.h>

#include "w1/aes.h"
#include "w1/aes_locl.h"
#include "w1/hobot_aes.h"
#include "w1/modes.h"
#include "w1/w1_ds28e1x_sha256.h"
#include "w1/w1_family.h"
#include "w1/w1_gpio.h"
#include "w1/w1.h"
#include "w1/w1_int.h"
#include "w1/x1_gpio.h"

#include "keros/keros.h"
#include "legacy-mtd-utils.h"

struct spi_flash *flash;

/*
 * This function computes the length argument for the erase command.
 * The length on which the command is to operate can be given in two forms:
 * 1. <cmd> offset len  - operate on <'offset',  'len')
 * 2. <cmd> offset +len - operate on <'offset',  'round_up(len)')
 * If the second form is used and the length doesn't fall on the
 * sector boundary, than it will be adjusted to the next sector boundary.
 * If it isn't in the flash, the function will fail (return -1).
 * Input:
 *    arg: length specification (i.e. both command arguments)
 * Output:
 *    len: computed length for operation
 * Return:
 *    1: success
 *   -1: failure (bad format, bad address).
 */
static int sf_parse_len_arg(char *arg, ulong *len)
{
	char *ep;
	char round_up_len; /* indicates if the "+length" form used */
	ulong len_arg;

	round_up_len = 0;
	if (*arg == '+') {
		round_up_len = 1;
		++arg;
	}

	len_arg = simple_strtoul(arg, &ep, 16);
	if (ep == arg || *ep != '\0')
		return -1;

	if (round_up_len && flash->sector_size > 0)
		*len = ROUND(len_arg, flash->sector_size);
	else
		*len = len_arg;

	return 1;
}

/**
 * This function takes a byte length and a delta unit of time to compute the
 * approximate bytes per second
 *
 * @param len		amount of bytes currently processed
 * @param start_ms	start time of processing in ms
 * @return bytes per second if OK, 0 on error
 */
static ulong bytes_per_second(unsigned int len, ulong start_ms)
{
	/* less accurate but avoids overflow */
	if (len >= ((unsigned int) -1) / 1024)
		return len / (max(get_timer(start_ms) / 1024, 1UL));
	else
		return 1024 * len / max(get_timer(start_ms), 1UL);
}

static int do_spi_flash_probe(int argc, char *const argv[])
{
	unsigned int bus = CONFIG_SF_DEFAULT_BUS;
	unsigned int cs = CONFIG_SF_DEFAULT_CS;
	/* In DM mode, defaults speed and mode will be taken from DT */
	unsigned int speed = CONFIG_SF_DEFAULT_SPEED;
	unsigned int mode = CONFIG_SF_DEFAULT_MODE;
	char *endp;
#ifdef CONFIG_DM_SPI_FLASH
	struct udevice *new, *bus_dev;
	int ret;
#else
	struct spi_flash *new;
#endif

	if (argc >= 2) {
		cs = simple_strtoul(argv[1], &endp, 0);
		if (*argv[1] == 0 || (*endp != 0 && *endp != ':'))
			return -1;
		if (*endp == ':') {
			if (endp[1] == 0)
				return -1;

			bus = cs;
			cs = simple_strtoul(endp + 1, &endp, 0);
			if (*endp != 0)
				return -1;
		}
	}

	if (argc >= 3) {
		speed = simple_strtoul(argv[2], &endp, 0);
		if (*argv[2] == 0 || *endp != 0)
			return -1;
	}
	if (argc >= 4) {
		mode = simple_strtoul(argv[3], &endp, 16);
		if (*argv[3] == 0 || *endp != 0)
			return -1;
	}

#ifdef CONFIG_DM_SPI_FLASH
	/* Remove the old device, otherwise probe will just be a nop */
	ret = spi_find_bus_and_cs(bus, cs, &bus_dev, &new);
	if (!ret) {
		device_remove(new, DM_REMOVE_NORMAL);
	}
	flash = NULL;
	ret = spi_flash_probe_bus_cs(bus, cs, speed, mode, &new);
	if (ret) {
		printf("Failed to initialize SPI flash at %u:%u (error %d)\n",
		       bus, cs, ret);
		return 1;
	}

	flash = dev_get_uclass_priv(new);
#else
	if (flash)
		spi_flash_free(flash);

	new = spi_flash_probe(bus, cs, speed, mode);
	flash = new;
	if (!new) {
		printf("Failed to initialize SPI flash at %u:%u\n", bus, cs);
		return 1;
	}
#endif

	return 0;
}

/**
 * Write a block of data to SPI flash, first checking if it is different from
 * what is already there.
 *
 * If the data being written is the same, then *skipped is incremented by len.
 *
 * @param flash		flash context pointer
 * @param offset	flash offset to write
 * @param len		number of bytes to write
 * @param buf		buffer to write from
 * @param cmp_buf	read buffer to use to compare data
 * @param skipped	Count of skipped data (incremented by this function)
 * @return NULL if OK, else a string containing the stage which failed
 */
static const char *spi_flash_update_block(struct spi_flash *flash, u32 offset,
		size_t len, const char *buf, char *cmp_buf, size_t *skipped)
{
	char *ptr = (char *)buf;

	debug("offset=%#x, sector_size=%#x, len=%#zx\n",
	      offset, flash->sector_size, len);
	/* Read the entire sector so to allow for rewriting */
	if (spi_flash_read(flash, offset, flash->sector_size, cmp_buf))
		return "read";
	/* Compare only what is meaningful (len) */
	if (memcmp(cmp_buf, buf, len) == 0) {
		debug("Skip region %x size %zx: no change\n",
		      offset, len);
		*skipped += len;
		return NULL;
	}
	/* Erase the entire sector */
	if (spi_flash_erase(flash, offset, flash->sector_size))
		return "erase";
	/* If it's a partial sector, copy the data into the temp-buffer */
	if (len != flash->sector_size) {
		memcpy(cmp_buf, buf, len);
		ptr = cmp_buf;
	}
	/* Write one complete sector */
	if (spi_flash_write(flash, offset, flash->sector_size, ptr))
		return "write";

	return NULL;
}

/**
 * Update an area of SPI flash by erasing and writing any blocks which need
 * to change. Existing blocks with the correct data are left unchanged.
 *
 * @param flash		flash context pointer
 * @param offset	flash offset to write
 * @param len		number of bytes to write
 * @param buf		buffer to write from
 * @return 0 if ok, 1 on error
 */
static int spi_flash_update(struct spi_flash *flash, u32 offset,
		size_t len, const char *buf)
{
	const char *err_oper = NULL;
	char *cmp_buf;
	const char *end = buf + len;
	size_t todo;		/* number of bytes to do in this pass */
	size_t skipped = 0;	/* statistics */
	const ulong start_time = get_timer(0);
	size_t scale = 1;
	const char *start_buf = buf;
	ulong delta;

	if (end - buf >= 200)
		scale = (end - buf) / 100;
	cmp_buf = memalign(ARCH_DMA_MINALIGN, flash->sector_size);
	if (cmp_buf) {
		ulong last_update = get_timer(0);

		for (; buf < end && !err_oper; buf += todo, offset += todo) {
			todo = min_t(size_t, end - buf, flash->sector_size);
			if (get_timer(last_update) > 100) {
				printf("   \rUpdating, %zu%% %lu B/s",
				       100 - (end - buf) / scale,
					bytes_per_second(buf - start_buf,
							 start_time));
				last_update = get_timer(0);
			}
			err_oper = spi_flash_update_block(flash, offset, todo,
					buf, cmp_buf, &skipped);
		}
	} else {
		err_oper = "malloc";
	}
	free(cmp_buf);
	putc('\r');
	if (err_oper) {
		printf("SPI flash failed in %s step\n", err_oper);
		return 1;
	}

	delta = get_timer(start_time);
	printf("%zu bytes written, %zu bytes skipped", len - skipped,
	       skipped);
	printf(" in %ld.%lds, speed %ld B/s\n",
	       delta / 1000, delta % 1000, bytes_per_second(len, start_time));

	return 0;
}

static int do_spi_flash_read_write(int argc, char *const argv[])
{
	unsigned long addr;
	void *buf;
	char *endp;
	int ret = 1;
	int dev = 0;
	loff_t offset, len, maxsize;

	if (argc < 3)
		return -1;

	addr = simple_strtoul(argv[1], &endp, 16);
	if (*argv[1] == 0 || *endp != 0)
		return -1;

	if (mtd_arg_off_size(argc - 2, &argv[2], &dev, &offset, &len,
			     &maxsize, MTD_DEV_TYPE_NOR, flash->size))
		return -1;

	/* Consistency checking */
	if (offset + len > flash->size) {
		printf("ERROR: attempting %s past flash size (%#x)\n",
		       argv[0], flash->size);
		return 1;
	}

	buf = map_physmem(addr, len, MAP_WRBACK);
	if (!buf && addr) {
		puts("Failed to map physical memory\n");
		return 1;
	}

	if (strcmp(argv[0], "update") == 0) {
		ret = spi_flash_update(flash, offset, len, buf);
	} else if (strncmp(argv[0], "read", 4) == 0 ||
			strncmp(argv[0], "write", 5) == 0) {
		int read;

		read = strncmp(argv[0], "read", 4) == 0;
		if (read)
			ret = spi_flash_read(flash, offset, len, buf);
		else
			ret = spi_flash_write(flash, offset, len, buf);

		printf("SF: %zu bytes @ %#x %s: ", (size_t)len, (u32)offset,
		       read ? "Read" : "Written");
		if (ret)
			printf("ERROR %d\n", ret);
		else
			printf("OK\n");
	}

	unmap_physmem(buf, len);

	return ret == 0 ? 0 : 1;
}

static int do_spi_flash_erase(int argc, char *const argv[])
{
	int ret;
	int dev = 0;
	loff_t offset, len, maxsize;
	ulong size;

	if (argc < 3)
		return -1;

	if (mtd_arg_off(argv[1], &dev, &offset, &len, &maxsize,
			MTD_DEV_TYPE_NOR, flash->size))
		return -1;

	ret = sf_parse_len_arg(argv[2], &size);
	if (ret != 1)
		return -1;

	/* Consistency checking */
	if (offset + size > flash->size) {
		printf("ERROR: attempting %s past flash size (%#x)\n",
		       argv[0], flash->size);
		return 1;
	}

	ret = spi_flash_erase(flash, offset, size);
	printf("SF: %zu bytes @ %#x Erased: %s\n", (size_t)size, (u32)offset,
	       ret ? "ERROR" : "OK");

	return ret == 0 ? 0 : 1;
}

static int do_spi_protect(int argc, char *const argv[])
{
	int ret = 0;
	loff_t start, len;
	bool prot = false;

	if (argc != 4)
		return -1;

	if (!str2off(argv[2], &start)) {
		puts("start sector is not a valid number\n");
		return 1;
	}

	if (!str2off(argv[3], &len)) {
		puts("len is not a valid number\n");
		return 1;
	}

	if (strcmp(argv[1], "lock") == 0)
		prot = true;
	else if (strcmp(argv[1], "unlock") == 0)
		prot = false;
	else
		return -1;  /* Unknown parameter */

	ret = spi_flash_protect(flash, start, len, prot);

	return ret == 0 ? 0 : 1;
}

#ifdef CONFIG_CMD_SF_TEST
enum {
	STAGE_ERASE,
	STAGE_CHECK,
	STAGE_WRITE,
	STAGE_READ,

	STAGE_COUNT,
};

static char *stage_name[STAGE_COUNT] = {
	"erase",
	"check",
	"write",
	"read",
};

struct test_info {
	int stage;
	int bytes;
	unsigned base_ms;
	unsigned time_ms[STAGE_COUNT];
};

static void show_time(struct test_info *test, int stage)
{
	uint64_t speed;	/* KiB/s */
	int bps;	/* Bits per second */

	speed = (long long)test->bytes * 1000;
	if (test->time_ms[stage])
		do_div(speed, test->time_ms[stage] * 1024);
	bps = speed * 8;

	printf("%d %s: %u ticks, %d KiB/s %d.%03d Mbps\n", stage,
	       stage_name[stage], test->time_ms[stage],
	       (int)speed, bps / 1000, bps % 1000);
}

static void spi_test_next_stage(struct test_info *test)
{
	test->time_ms[test->stage] = get_timer(test->base_ms);
	show_time(test, test->stage);
	test->base_ms = get_timer(0);
	test->stage++;
}

/**
 * Run a test on the SPI flash
 *
 * @param flash		SPI flash to use
 * @param buf		Source buffer for data to write
 * @param len		Size of data to read/write
 * @param offset	Offset within flash to check
 * @param vbuf		Verification buffer
 * @return 0 if ok, -1 on error
 */
static int spi_flash_test(struct spi_flash *flash, uint8_t *buf, ulong len,
			   ulong offset, uint8_t *vbuf)
{
	struct test_info test;
	int i;

	printf("SPI flash test:\n");
	memset(&test, '\0', sizeof(test));
	test.base_ms = get_timer(0);
	test.bytes = len;
	if (spi_flash_erase(flash, offset, len)) {
		printf("Erase failed\n");
		return -1;
	}
	spi_test_next_stage(&test);

	if (spi_flash_read(flash, offset, len, vbuf)) {
		printf("Check read failed\n");
		return -1;
	}
	for (i = 0; i < len; i++) {
		if (vbuf[i] != 0xff) {
			printf("Check failed at %d\n", i);
			print_buffer(i, vbuf + i, 1,
				     min_t(uint, len - i, 0x40), 0);
			return -1;
		}
	}
	spi_test_next_stage(&test);

	if (spi_flash_write(flash, offset, len, buf)) {
		printf("Write failed\n");
		return -1;
	}
	memset(vbuf, '\0', len);
	spi_test_next_stage(&test);

	if (spi_flash_read(flash, offset, len, vbuf)) {
		printf("Read failed\n");
		return -1;
	}
	spi_test_next_stage(&test);

	for (i = 0; i < len; i++) {
		if (buf[i] != vbuf[i]) {
			printf("Verify failed at %d, good data:\n", i);
			print_buffer(i, buf + i, 1,
				     min_t(uint, len - i, 0x40), 0);
			printf("Bad data:\n");
			print_buffer(i, vbuf + i, 1,
				     min_t(uint, len - i, 0x40), 0);
			return -1;
		}
	}
	printf("Test passed\n");
	for (i = 0; i < STAGE_COUNT; i++)
		show_time(&test, i);

	return 0;
}

static int do_spi_flash_test(int argc, char *const argv[])
{
	unsigned long offset;
	unsigned long len;
	uint8_t *buf, *from;
	char *endp;
	uint8_t *vbuf;
	int ret;

	if (argc < 3)
		return -1;
	offset = simple_strtoul(argv[1], &endp, 16);
	if (*argv[1] == 0 || *endp != 0)
		return -1;
	len = simple_strtoul(argv[2], &endp, 16);
	if (*argv[2] == 0 || *endp != 0)
		return -1;

	vbuf = memalign(ARCH_DMA_MINALIGN, len);
	if (!vbuf) {
		printf("Cannot allocate memory (%lu bytes)\n", len);
		return 1;
	}
	buf = memalign(ARCH_DMA_MINALIGN, len);
	if (!buf) {
		free(vbuf);
		printf("Cannot allocate memory (%lu bytes)\n", len);
		return 1;
	}

	from = map_sysmem(CONFIG_SYS_TEXT_BASE, 0);
	memcpy(buf, from, len);
	ret = spi_flash_test(flash, buf, len, offset, vbuf);
	free(vbuf);
	free(buf);
	if (ret) {
		printf("Test failed\n");
		return 1;
	}

	return 0;
}
#endif /* CONFIG_CMD_SF_TEST */

static int do_spi_flash(cmd_tbl_t *cmdtp, int flag, int argc,
			char *const argv[])
{
	const char *cmd;
	int ret;

	/* need at least two arguments */
	if (argc < 2)
		goto usage;

	cmd = argv[1];
	--argc;
	++argv;

	if (strcmp(cmd, "probe") == 0) {
		ret = do_spi_flash_probe(argc, argv);
		goto done;
	}

	/* The remaining commands require a selected device */
	if (!flash) {
		puts("No SPI flash selected. Please run `sf probe'\n");
		return 1;
	}

	if (strcmp(cmd, "read") == 0 || strcmp(cmd, "write") == 0 ||
	    strcmp(cmd, "update") == 0)
		ret = do_spi_flash_read_write(argc, argv);
	else if (strcmp(cmd, "erase") == 0)
		ret = do_spi_flash_erase(argc, argv);
	else if (strcmp(cmd, "protect") == 0)
		ret = do_spi_protect(argc, argv);
#ifdef CONFIG_CMD_SF_TEST
	else if (!strcmp(cmd, "test"))
		ret = do_spi_flash_test(argc, argv);
#endif
	else
		ret = -1;

done:
	if (ret != -1)
		return ret;

usage:
	return CMD_RET_USAGE;
}

#ifdef CONFIG_CMD_SF_TEST
#define SF_TEST_HELP "\nsf test offset len		" \
		"- run a very basic destructive test"
#else
#define SF_TEST_HELP
#endif

#define  SECURE_IC_ID     0x4B
#define  HEADER_FIX       0x31764750
#define  SECURE_KEY_TYPE  0x00000003
#define  SN_TYPE          0x00000001
#define  PACK_LEN         16
#define  SECURE_KEY_LEN   1024    //
#define  SN_LEN           64    //
#define  SN_OFFSET   66    /* veeprom // 0x4400 + 66*/
#define SECURE_SN_LEN       (32)
#define BOOT_ARG_DDR_ADDR    0xC800000    /* 200M ddr for save boot args */
#define BOOT_ARGINFO_BLOCK   0x10000      /* min block 64K */
static uint8_t sn_buf[SN_LEN];

enum pack_index {
    HEADER_INDEX =0,
    TYPE_INDEX,
    LENGTH_INDEX,
    CRC_INDEX,
    DATA_INDEX,
    MAX_STATES = 0x0F
};

struct w1_master                 *master_total = NULL;
struct w1_bus_master              bus_master;
struct w1_gpio_platform_data      pdata_sf;


char aes_key[HOBOT_AES_BLOCK_SIZE] = { 0x3f, 0x48, 0x15, 0x16, 0x6f, 0xae, 0xd2, 0xa6, 0xe6, 0x27, 0x15, 0x69, 0x09, 0xcf, 0x7a, 0x3c};

static int mark_register_cnt =0;

int  check_Crc(uint16_t crc_start, unsigned char * Data, int len)
{
   uint32_t  mCrc = crc_start;
   uint32_t  i,j;

   for(j = 0; j < len; j++){
       mCrc = mCrc^(uint16_t)(Data[j]) << 8;    //       mCrc = mCrc^(uint16_t)(Data.at(j)) << 8;
       for (i=8; i!=0; i--){
           if (mCrc & 0x8000)
               mCrc = mCrc << 1 ^ 0x1021;
           else
               mCrc = mCrc << 1;
       }
   }
   return mCrc;
}


int w1_init_setup(void)
{
    if (w1_ds28e1x_init() < 0) {
        printf("ds28e1x init fail !!!\n");
        return -1;
    }

    memset(&bus_master,0x0,sizeof(struct w1_bus_master));
    memset(&pdata_sf,0x0,sizeof(struct w1_gpio_platform_data));
    master_total = w1_gpio_probe(&bus_master, &pdata_sf);

    if (!master_total) {
        printf("w1_gpio_probe fail !!!\n");
        return -1;
    }

    if (w1_process(master_total) < 0) {
        printf("ds28e1x read ID fail !!!\n");
        return -1;
    }

    if (w1_master_setup_slave(master_total,SECURE_IC_ID,NULL,NULL) < 0) {
        printf("ds28e1x setup_device fail !!!\n");
        return -1;
    }

    return 0;
}


int do_burn_secure(cmd_tbl_t *cmdtp, int flag, int argc,
                char * const argv[])
{
	int ret = 0, has_auth;
	unsigned long offset;
	uint32_t header,type,d_length,crc,c_crc;
	uint8_t  package[SECURE_KEY_LEN] = {0};
	uint8_t  secure_key[SECURE_KEY_LEN] ={0};
	uint32_t *p_pack = (uint32_t *)package;
	uint8_t key_note[HOBOT_AES_BLOCK_SIZE] = {0x3f, 0x48, 0x15, 0x16, 0x6f, 0xae, 0xd2, 0xa6, 0xe6, 0x27, 0x15, 0x69, 0x09, 0xcf, 0x7a, 0x3c};
	uint8_t real_key[32] = {0};

	if (!mark_register_cnt)
	ret = w1_init_setup();

	if (ret < 0) {
		printf("burn_key_w1_init_setup_error\n");
		printf("burn_key_failed\n");
		return 0;
	}
	mark_register_cnt =1;

	offset = simple_strtoul(argv[1], NULL, 16);    /* secure data addr in ddr*/

	memcpy(&package[0],(uint8_t *)offset,sizeof(package));

	header = *p_pack;
	type =  *(p_pack + TYPE_INDEX);
	d_length =  *(p_pack + LENGTH_INDEX);
	crc = *(p_pack + CRC_INDEX);

	if (header != HEADER_FIX) {
		printf("burn_key_header_error\n");
		printf("burn_key_failed\n");
		return 0;
	}

	if (type != SECURE_KEY_TYPE) {
		printf("burn_key_type_error\n");
		printf("burn_key_failed\n");
		return 0;
	}

	if (d_length <= SECURE_KEY_LEN - PACK_LEN) {
		memcpy(&secure_key[0],(uint8_t *)(p_pack + DATA_INDEX),d_length);
	} else {
		printf("burn_key_length_error, length > %dbyte\n", SECURE_KEY_LEN - PACK_LEN);
		printf("burn_key_failed\n");
		return 0;
	}

	c_crc=check_Crc(0,&package[PACK_LEN],d_length);
	if (crc !=c_crc){
		printf("burn_key_crc_error\n");
		printf("burn_key_failed\n");
		return 0;
	}

	ret = w1_master_is_write_auth_mode(master_total, SECURE_IC_ID, &has_auth);
	if (ret != 0) {
		printf("w1_master_is_write_auth_mode failed\n");
		printf("burn_key_failed\n");
		return 0;
	}

	/* decrypt to realy key */
	hb_aes_decrypt((char *)&secure_key[32], (char *)key_note, (char *)real_key, 32);
	memcpy(&secure_key[32], real_key, 32);

	/* load the secure key only */
	ret = w1_master_load_key(master_total, SECURE_IC_ID, (char *)secure_key, NULL);
	if (ret != 0) {
		printf("burn_key_w1_master_load_key_error\n");
		printf("burn_key_failed\n");
		return 0;
	}

	/* load the usr_data */
	ret = w1_master_auth_write_usr_mem(master_total, SECURE_IC_ID,
	    (char *)secure_key);
	if (ret != 0) {
		printf("burn_key_w1_master_auth_write_usr_mem_error\n");
		printf("burn_key_failed\n");
		return 0;
	}

	if (has_auth) {
		ret = w1_master_auth_write_block_protection(master_total, SECURE_IC_ID, real_key);
		if (ret != 0) {
			printf("w1_master_auth_write_block_protection\n");
			printf("burn_key_failed\n");
			return 0;
		}
	} else {
		/* first set the auth mode */
		ret = w1_master_set_write_auth_mode(master_total, SECURE_IC_ID);
		if (ret != 0) {
			printf("burn_w1_master_set_write_auth_mode_error\n");
			printf("burn_key_failed\n");
			return 0;
		}
	}

	printf("burn_key_succeeded\n");

	return 0;
}

int do_burn_sn(cmd_tbl_t *cmdtp, int flag, int argc,
                    char * const argv[])
{
	unsigned int bus = CONFIG_SF_DEFAULT_BUS;
	unsigned int cs = CONFIG_SF_DEFAULT_CS;
	unsigned int speed = CONFIG_SF_DEFAULT_SPEED;
	unsigned int mode = CONFIG_SF_DEFAULT_MODE;
	int ret = 0;
	uint32_t header,type,d_length,crc,c_crc;
	uint8_t  package[SN_LEN] = {0};
	uint32_t *p_pack = (uint32_t *)package;
	unsigned long offset =0;
	unsigned long *buf = (unsigned long *)&sn_buf[0];
	int boot_mode = hb_boot_mode_get();

	memset(buf, 0, SN_LEN);
	offset = simple_strtoul(argv[1], NULL, 16);    /* sn data addr in ddr*/
	memcpy(&package[0],(uint8_t *)offset,sizeof(package));
	if (boot_mode == PIN_2ND_NOR) {
		flash = spi_flash_probe(bus, cs, speed, mode);
		if (!flash) {
			printf("burn_sn_initialize_spi_flash_error\n");
			printf("burn_sn_failed\n");
			return 0;
		}
	}
	header = *p_pack;
	type =	*(p_pack + TYPE_INDEX);
	d_length =	*(p_pack + LENGTH_INDEX);
	crc = *(p_pack + CRC_INDEX);

	if (header != HEADER_FIX) {
		printf("burn_sn_header_error\n");
		printf("burn_sn_failed\n");
		return 0;
	}
	if (type != SN_TYPE) {
		printf("burn_sn_type_error\n");
		printf("burn_sn_failed\n");
		return 0;
	}

	if (d_length <= SN_LEN - PACK_LEN) {
		//memcpy(&board_sn[0],(uint8_t *)(p_pack + DATA_INDEX),d_length);
		memcpy(buf + 1,(uint8_t *)(p_pack + DATA_INDEX),d_length);
	} else {
		printf("burn_sn_length_error,sn > %dbyte\n", SN_LEN - PACK_LEN);
		printf("burn_sn_failed\n");
		return 0;
	}

	c_crc=check_Crc(0,&package[PACK_LEN],d_length);
	if (crc !=c_crc) {
		printf("burn_sn_crc_error\n");
		printf("burn_sn_failed\n");
		return 0;
	}

	memcpy((uint32_t*)buf,&d_length,sizeof(uint32_t));
	/*
	ret = spi_flash_erase(flash,SECURE_SNINFO_ADDR,flash->sector_size);
	if (ret !=0) {
		printf("burn_sn_erase_flash_error\n");
		printf("burn_sn_failed\n");
		return 0;
	}
	*/

	ret = veeprom_clear(SN_OFFSET, SN_LEN);
	if ((boot_mode == PIN_2ND_NOR) || (boot_mode == PIN_2ND_NAND)) {
		if (ret !=0) {
			printf("burn_sn_erase_flash_error\n");
			printf("burn_sn_failed\n");
			return 0;
		}
	} else {
		if (ret !=1) {
			printf("burn_sn_erase_flash_error\n");
			printf("burn_sn_failed\n");
			return 0;
		}
	}

	ret = veeprom_write(SN_OFFSET, (const char *)buf, SN_LEN);
	if ((boot_mode == PIN_2ND_NOR) || (boot_mode == PIN_2ND_NAND)) {
		if (ret !=0) {
			printf("burn_sn_write_flash_error\n");
			printf("burn_sn_failed\n");
			return 0;
		}
	} else {
		if (ret !=1) {
			printf("burn_sn_write_flash_error\n");
			printf("burn_sn_failed\n");
			return 0;
		}
	}
/*
    ret = spi_flash_write(flash,SECURE_SNINFO_ADDR,flash->sector_size,(uint32_t*)buf);
    printf("burn sn:%send\n",buf);

	if (ret !=0) {
		printf("burn_sn_write_flash_error\n");
		printf("burn_sn_failed\n");
		return 0;
	}
*/
	printf("burn_sn_succeeded\n");
	return 0;
}


int do_get_sn(cmd_tbl_t *cmdtp, int flag, int argc,
										char * const argv[])
{
	int ret =0;
	unsigned int bus = CONFIG_SF_DEFAULT_BUS;
	unsigned int cs = CONFIG_SF_DEFAULT_CS;
	unsigned int speed = CONFIG_SF_DEFAULT_SPEED;
	unsigned int mode = CONFIG_SF_DEFAULT_MODE;
	unsigned int sn_len = 0x0;
	uint8_t  board_sn[SN_LEN] = {0};
	unsigned long *buf = (unsigned long *)&sn_buf[0];
	int boot_mode = hb_boot_mode_get();

	memset(buf, 0, SN_LEN);

	if (boot_mode == PIN_2ND_NOR) {
		flash = spi_flash_probe(bus, cs, speed, mode);
		if (!flash) {
			printf("burn_sn_initialize_spi_flash_error\n");
			printf("burn_sn_failed\n");
			return 0;
		}
	}

	ret = veeprom_read(SN_OFFSET, (char *)buf, SN_LEN);
	if ((boot_mode == PIN_2ND_NOR) || (boot_mode == PIN_2ND_NAND)) {
		if (ret !=0) {
			printf("read_sn_failed\n");
			return 0;
		}
	} else {
		if (ret !=1) {
			printf("read_sn_failed\n");
			return 0;
		}
	}
	sn_len = *buf;
	memcpy(&board_sn[0],(uint8_t *)(buf+1),sn_len);

	printf("len = %d, gsn:%send\n",sn_len, board_sn);

	return 0;
}

int do_get_sid(cmd_tbl_t *cmdtp, int flag, int argc,
										char * const argv[])
{
    unsigned char sec_id[8] = {0};
    int i = 0;
    int ret = 0;
    unsigned char total = 0;

    if (!mark_register_cnt) {
        ret = w1_init_setup();
        if (!ret)
            mark_register_cnt =1;
    }

    if (mark_register_cnt)
        w1_master_get_rom_id(master_total, SECURE_IC_ID, (char *)sec_id);

    printf("gsid: ");
    for (i=0; i<8; i++) {
        printf("%d ", sec_id[i]);
        total += sec_id[i];
    }
    printf("%d ", total);
    printf("end\n");

	return 0;
}

int do_burn_keros(cmd_tbl_t *cmdtp, int flag, int argc,
										char * const argv[])
{
	int ret = 0;
	unsigned long offset;
	uint8_t page, encrytion;
	uint32_t old_password, new_password;
	uint32_t header,type,d_length,crc,c_crc;
	uint8_t  package[SECURE_KEY_LEN] = {0};
	uint8_t  secure_key[SECURE_KEY_LEN] ={0};
	uint32_t *p_pack = (uint32_t *)package;

	if (!mark_register_cnt)
		ret = keros_init();

	if (ret < 0) {
		printf("keros init failed\n");
		printf("burn_key_failed\n");
		return 0;
	}
	mark_register_cnt =1;

	offset = simple_strtoul(argv[1], NULL, 16);    /* secure data addr in ddr*/

	memcpy(&package[0],(uint8_t *)offset,sizeof(package));

	header = *p_pack;
	type =  *(p_pack + TYPE_INDEX);
	d_length =  *(p_pack + LENGTH_INDEX);
	crc = *(p_pack + CRC_INDEX);

	if (header != HEADER_FIX) {
		printf("burn_key_header_error\n");
		printf("burn_key_failed\n");
		return 0;
	}

	if (type != SECURE_KEY_TYPE) {
		printf("burn_key_type_error\n");
		printf("burn_key_failed\n");
		return 0;
	}

	if (d_length <= SECURE_KEY_LEN - PACK_LEN) {
		page = *(p_pack + DATA_INDEX);
		encrytion = *(p_pack + DATA_INDEX + 1);
		old_password = *(p_pack + DATA_INDEX + 2);
		new_password = *(p_pack + DATA_INDEX + 3);
		memcpy(&secure_key[0], (uint8_t *)(p_pack + DATA_INDEX + 4), d_length - 4*4);
	} else {
		printf("burn_key_length_error, length > %dbyte\n", SECURE_KEY_LEN - PACK_LEN);
		printf("burn_key_failed\n");
		return 0;
	}

	debug("page： %d\n", page);
	debug("encrytion: %d\n", encrytion);
	debug("old password: %d\n", old_password);
	debug("new password: %d\n", new_password);
	debug("content:\n");
	for (int i = 0; i < d_length; ++i) {
		debug("%x", secure_key[i]);
	}
	debug("\n");

	c_crc = check_Crc(0, &package[PACK_LEN], d_length);
	if (crc !=c_crc){
		printf("burn_key_crc_error\n");
		printf("burn_key_failed\n");
		return 0;
	}

	ret = keros_authentication();
	if (ret != 0) {
		printf("keros authentication failed\n");
		printf("burn_key_failed\n");
		return 0;
	}

	ret = keros_pwchg(page, old_password, new_password);
	if (ret != 0) {
		printf("keros password chang faild\n");
		printf("burn_key_failed\n");
	}

	/* load the secure key only */
	ret = keros_write_key(new_password, page, secure_key, encrytion);
	if (ret != 0) {
		printf("write key to eeprom failed\n");
		printf("burn_key_failed\n");
		return 0;
	}

	printf("burn_key_succeeded\n");
	return 0;
}
U_BOOT_CMD(
	sf,	5,	1,	do_spi_flash,
	"SPI flash sub-system",
	"probe [[bus:]cs] [hz] [mode]	- init flash device on given SPI bus\n"
	"				  and chip select\n"
	"sf read addr offset|partition len	- read `len' bytes starting at\n"
	"				          `offset' or from start of mtd\n"
	"					  `partition'to memory at `addr'\n"
	"sf write addr offset|partition len	- write `len' bytes from memory\n"
	"				          at `addr' to flash at `offset'\n"
	"					  or to start of mtd `partition'\n"
	"sf erase offset|partition [+]len	- erase `len' bytes from `offset'\n"
	"					  or from start of mtd `partition'\n"
	"					 `+len' round up `len' to block size\n"
	"sf update addr offset|partition len	- erase and write `len' bytes from memory\n"
	"					  at `addr' to flash at `offset'\n"
	"					  or to start of mtd `partition'\n"
	"sf protect lock/unlock sector len	- protect/unprotect 'len' bytes starting\n"
	"					  at address 'sector'\n"
	SF_TEST_HELP
);

U_BOOT_CMD(
        burn, 2, 0, do_burn_secure,
        "burn Secure data",
        "burn addr\r\n"
        ""
);

U_BOOT_CMD(
	sn, 2, 0, do_burn_sn,
	"sn SN data",
	"sn addr\r\n"
	""
);

U_BOOT_CMD(
	gsn, 2, 0, do_get_sn,
	"get SN data",
	"gsn \r\n"
	""
);

U_BOOT_CMD(
	gsid, 2, 0, do_get_sid,
	"get secret-ic rom-id data",
	"gsid \r\n"
	""
);

U_BOOT_CMD(
	keros, 2, 0, do_burn_keros,
	"burn keros secure chip",
	"keros \r\n"
	""
);
