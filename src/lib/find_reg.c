/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Tom St Denis <tom.stdenis@amd.com>
 *
 */
#include "umr.h"

uint32_t umr_find_reg(struct umr_asic *asic, char *regname)
{
	int i, j;

	for (i = 0; i < asic->no_blocks; i++)
		for (j = 0; j < asic->blocks[i]->no_regs; j++)
			if (!strcmp(asic->blocks[i]->regs[j].regname, regname))
				return asic->blocks[i]->regs[j].addr;
	fprintf(stderr, "[BUG]: reg [%s] not found on asic [%s]\n", regname, asic->asicname);
	return 0xFFFFFFFF;
}

struct umr_reg *umr_find_reg_data_by_ip(struct umr_asic *asic, char *ip, char *regname)
{
	int i, j;

	for (i = 0; i < asic->no_blocks; i++) {
		if (ip && memcmp(asic->blocks[i]->ipname, ip, strlen(ip))) continue;
		for (j = 0; j < asic->blocks[i]->no_regs; j++)
			if (!strcmp(asic->blocks[i]->regs[j].regname, regname))
				return &asic->blocks[i]->regs[j];
	}
	fprintf(stderr, "[BUG]: reg [%s] not found on asic [%s]\n", regname, asic->asicname);
	return NULL;
}

struct umr_reg *umr_find_reg_data(struct umr_asic *asic, char *regname)
{
	return umr_find_reg_data_by_ip(asic, NULL, regname);
}

struct umr_reg *umr_find_reg_by_addr(struct umr_asic *asic, uint64_t addr, struct umr_ip_block **ip)
{
	int i, j;

	if (asic->mmio_accel.reglist && asic->mmio_accel.reglist[addr]) {
		if (ip && asic->mmio_accel.iplist)
			*ip = asic->mmio_accel.iplist[addr];
		return asic->mmio_accel.reglist[addr];
	}

	for (i = 0; i < asic->no_blocks; i++)
	for (j = 0; j < asic->blocks[i]->no_regs; j++)
		if (asic->blocks[i]->regs[j].type == REG_MMIO && asic->blocks[i]->regs[j].addr == addr) {
			if (ip)
				*ip = asic->blocks[i];
			return &asic->blocks[i]->regs[j];
		}
	return NULL;
}

char *umr_reg_name(struct umr_asic *asic, uint64_t addr)
{
	struct umr_reg *reg;
	struct umr_ip_block *ip;
	static char name[512];

	reg = umr_find_reg_by_addr(asic, addr, &ip);
	if (ip && reg) {
		sprintf(name, "%s%s.%s%s", RED, ip->ipname, reg->regname, RST);
		return name;
	} else {
		return "<unknown>";
	}
}
