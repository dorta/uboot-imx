/*
 * Copyright (C) 2023 Variscite Ltd.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */
#include <common.h>
#include <command.h>
#include <dm.h>
#include <i2c.h>
#include <asm/io.h>
#include <cpu_func.h>
#include <u-boot/crc.h>
#include <asm/arch-imx9/ddr.h>

#include "imx9_eeprom.h"

struct mx9_ddr_adjust {
	const char * name;
	struct dram_cfg_param * cfg_param;
	unsigned int cfg_num;
};

static int var_eeprom_get_dev(struct udevice **devp)
{
	int ret;
	struct udevice *bus;

	ret = uclass_get_device_by_name(UCLASS_I2C, VAR_SOM_EEPROM_I2C_NAME, &bus);
	if (ret) {
		debug("%s: No EEPROM I2C bus '%s'\n", __func__, VAR_SOM_EEPROM_I2C_NAME);
		return ret;
	}

	ret = dm_i2c_probe(bus, VAR_SOM_EEPROM_I2C_ADDR, 0, devp);
	if (ret) {
		debug("%s: I2C EEPROM probe failed\n", __func__);
		return ret;
	}

	return 0;
}

int var_eeprom_read_header(struct var_eeprom *e)
{
	int ret;
	struct udevice *dev;

	ret = var_eeprom_get_dev(&dev);
	if (ret) {
		debug("%s: Failed to detect I2C EEPROM\n", __func__);
		return ret;
	}

	/* Read EEPROM header to memory */
	ret = dm_i2c_read(dev, 0, (void *)e, sizeof(*e));
	if (ret) {
		debug("%s: EEPROM read failed, ret=%d\n", __func__, ret);
		return ret;
	}

	return 0;
}

int var_eeprom_get_mac(struct var_eeprom *ep, u8 *mac)
{
	flush_dcache_all();
	if (!var_eeprom_is_valid(ep))
		return -1;

	memcpy(mac, ep->mac, sizeof(ep->mac));

	return 0;
}

int var_eeprom_get_dram_size(struct var_eeprom *ep, phys_size_t *size)
{
	/* No data in EEPROM - return default DRAM size */
	if (!var_eeprom_is_valid(ep)) {
		*size = DEFAULT_SDRAM_SIZE;
		return 0;
	}

	*size = (ep->dramsize * 128UL) << 20;
	return 0;
}

#ifndef CONFIG_SPL_BUILD
void var_eeprom_print_prod_info(struct var_eeprom *ep)
{
	u8 partnum[8] = {0};

	flush_dcache_all();

	if (!var_eeprom_is_valid(ep))
		return;

	/* Read first part of P/N  */
	memcpy(partnum, ep->partnum, sizeof(ep->partnum));

	/* Read second part of P/N  */
	if (ep->version >= 3)
		memcpy(partnum + sizeof(ep->partnum), ep->partnum2, sizeof(ep->partnum2));


#ifdef CONFIG_TARGET_MX93_VAR_SOM
	printf("\nPart number: VSM-MX93-%.*s\n", (int)sizeof(partnum), partnum);
#endif

	printf("Assembly: AS%.*s\n", (int)sizeof(ep->assembly), (char *)ep->assembly);

	printf("Production date: %.*s %.*s %.*s\n",
			4, /* YYYY */
			(char *)ep->date,
			3, /* MMM */
			((char *)ep->date) + 4,
			2, /* DD */
			((char *)ep->date) + 4 + 3);

	printf("Serial Number: %02x:%02x:%02x:%02x:%02x:%02x\n",
		ep->mac[0], ep->mac[1], ep->mac[2], ep->mac[3], ep->mac[4], ep->mac[5]);

	debug("EEPROM version: 0x%x\n", ep->version);
	debug("SOM features: 0x%x\n", ep->features);
	printf("SOM revision: 0x%x\n", ep->somrev);
	debug("DRAM size: %d GiB\n\n", (ep->dramsize * 128) / 1024);
}
#endif

#if defined(CONFIG_SPL_BUILD)
/*
 * Modify DRAM table based on adjustment table in EEPROM
 *
 * Assumption: register addresses in the adjustment table
 * follow the order of register addresses in the original table
 *
 * @adj_table_offset - offset of adjustment table from start of EEPROM
 * @adj_table_size   - number of rows in adjustment table
 * @table            - pointer to DDR table
 * @table_size       - number of rows in DDR table
 */
static void adjust_dram_table(u8 adj_table_offset, u8 adj_table_size,
				struct dram_cfg_param *table, u8 table_size)
{
	int i, j = 0;
	u8 off = adj_table_offset;
	struct dram_cfg_param adj_table_row;
	int ret;
	struct udevice *dev;

	/* Get EEPROM device */
	ret = var_eeprom_get_dev(&dev);
	if (ret) {
		debug("%s: Failed to detect I2C EEPROM\n", __func__);
		return;
	}

	/* Iterate over adjustment table */
	for (i = 0; i < adj_table_size; i++) {
		/* Read next entry from adjustment table */
		dm_i2c_read(dev, off,
			(uint8_t *)&adj_table_row, sizeof(adj_table_row));

		/* Iterate over DDR table and adjust it */
		for (; j < table_size; j++) {
			if (table[j].reg == adj_table_row.reg) {
				debug("Adjusting reg=0x%x val=0x%x\n",
					adj_table_row.reg, adj_table_row.val);
				table[j].val = adj_table_row.val;
				break;
			}
		}

		off += sizeof(adj_table_row);
	}
}

/*
 * Modify DRAM tables based on adjustment tables in EEPROM
 *
 * @e - pointer to EEPROM header structure
 * @d - pointer to DRAM configuration structure
  */
void var_eeprom_adjust_dram(struct var_eeprom *ep, struct dram_timing_info *d)
{
	int i;
	u16 adj_table_size[DRAM_TABLE_NUM];

	/* Aligned with Variscite SoM EEPROM DDR Adjust Tables */
	struct mx9_ddr_adjust mx9_adjust_table[] = {
		{ .name = "DDRC",	.cfg_param = d->ddrc_cfg,		.cfg_num = d->ddrc_cfg_num },
		{ .name = "DDR PHY",	.cfg_param = d->ddrphy_cfg,		.cfg_num = d->ddrphy_cfg_num },
		{ .name = "PIE",	.cfg_param = d->ddrphy_pie,		.cfg_num = d->ddrphy_pie_num },
		{ .name = "FSP0",	.cfg_param = d->fsp_msg[0].fsp_cfg, 	.cfg_num = d->fsp_msg[0].fsp_cfg_num },
		{ .name = "FSP0 2D",	.cfg_param = d->fsp_msg[1].fsp_cfg, 	.cfg_num = d->fsp_msg[1].fsp_cfg_num },
		{ .name = NULL },
	};

	if (!var_eeprom_is_valid(ep))
		return;

	/* Check EEPROM version - only version 2+ has DDR adjustment tables */
	if (ep->version < 2) {
		debug("EEPROM version is %d\n", ep->version);
		return;
	}

	debug("EEPROM offset table\n");
	for (i = 0; i < DRAM_TABLE_NUM + 1; i++)
		debug("off[%d]=%d\n", i, ep->off[i]);

	/* Calculate DRAM adjustment table sizes */
	for (i = 0; i < DRAM_TABLE_NUM && ep->off[i + 1] != 0; i++)
		adj_table_size[i] = (ep->off[i + 1] - ep->off[i]) /
				(sizeof(struct dram_cfg_param));

	debug("\nSizes table\n");
	for (i = 0; i < DRAM_TABLE_NUM; i++)
		debug("sizes[%d]=%d\n", i, adj_table_size[i]);

	/* Adjust all DDR Tables */
	for (i = 0; mx9_adjust_table[i].name != NULL; i++)
	{
		debug("\nAdjusting %s table: offset=%d, count=%d\n",
			mx9_adjust_table[i].name, ep->off[i], adj_table_size[i]);
		adjust_dram_table(ep->off[i], adj_table_size[i],
			mx9_adjust_table[i].cfg_param, mx9_adjust_table[i].cfg_num);
	}

	/* Adjust FSP */
	debug("fsp_drate: %d\n", ep->fsp_drate);
	d->fsp_msg[0].drate = ep->fsp_drate;
	d->fsp_msg[1].drate = ep->fsp_drate;
	d->fsp_table[0] = ep->fsp_drate;
}
#endif

int var_carrier_eeprom_read(const char * bus_name, int addr, struct var_carrier_eeprom *ep)
{
	int ret;
	struct udevice *bus;
	struct udevice *dev;

	ret = uclass_get_device_by_name(UCLASS_I2C, bus_name, &bus);
	if (ret) {
		debug("%s: No bus '%s'\n", __func__, bus_name);
		return ret;
	}

	ret = dm_i2c_probe(bus, addr, 0, &dev);
	if (ret) {
		debug("%s: Carrier EEPROM I2C probe failed\n", __func__);
		return ret;
	}

	/* Read EEPROM to memory */
	ret = dm_i2c_read(dev, 0, (void *)ep, sizeof(*ep));
	if (ret) {
		debug("%s: Carrier EEPROM read failed, ret=%d\n", __func__, ret);
		return ret;
	}

	return 0;
}

int var_carrier_eeprom_is_valid(struct var_carrier_eeprom *ep)
{
	u32 crc, crc_offset = offsetof(struct var_carrier_eeprom, crc);

	if (htons(ep->magic) != VAR_CARRIER_EEPROM_MAGIC) {
		debug("Invalid carrier EEPROM magic 0x%hx, expected 0x%hx\n",
			htons(ep->magic), VAR_CARRIER_EEPROM_MAGIC);
		return 0;
	}

	if (ep->struct_ver < 1) {
		printf("Invalid carrier EEPROM version 0x%hx\n", ep->struct_ver);
		return 0;
	}

	if (ep->struct_ver == 1)
		return 1;

	/* Only EEPROM structure above version 1 has CRC field */
	crc = crc32(0, (void *)ep, crc_offset);

	if (crc != ep->crc) {
		printf("Carrier EEPROM CRC mismatch (%08x != %08x)\n",
			crc, be32_to_cpu(ep->crc));
		return 0;
	}

	return 1;
}

/* Returns carrier board revision string via 'rev' argument.
 * For legacy carrier board revisions the "legacy" string is returned.
 * For new carrier board revisions the actual carrier revision is returned.
 * Symphony-Board 1.4 and below are legacy, 1.4a and above are new.
 * DT8MCustomBoard 1.4 and below are legacy, 2.0 and above are new.
 */
void var_carrier_eeprom_get_revision(struct var_carrier_eeprom *ep, char *rev, size_t size)
{
	if (var_carrier_eeprom_is_valid(ep))
		strncpy(rev, (const char *)ep->carrier_rev, size);
	else
		strncpy(rev, "legacy", size);
}
