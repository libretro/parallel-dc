/* The shil operations' canonical functions, their compile functions for
 * the recompilers (shil_chf[]), names and text. */
#include <stdio.h>
#include <stdint.h>

#include "types.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mmr.h"
#include "rec_config.h"
#include "shil.h"

#define SHIL_MODE 1
#include "shil_canonical.h"

#define SHIL_MODE 4
#include "shil_canonical.h"

#if FEAT_SHREC != DYNAREC_NONE
#define SHIL_MODE 3
#include "shil_canonical.h"
#endif

void shil_name_reg(char *buf, size_t size, Sh4RegType reg)
{
	const char *name = NULL;
	char tmp[32];
	if (reg >= reg_fr_0 && reg <= reg_xf_15)
		sprintf(tmp, "f%d", (int)(reg - reg_fr_0));
	else if (reg <= reg_r15)
		sprintf(tmp, "r%d", (int)reg);
	else if (reg <= reg_r7_Bank)
		sprintf(tmp, "r%db", (int)(reg - reg_r0_Bank));
	else
	{
		switch (reg)
		{
		case reg_sr_T: name = "sr.T"; break;
		case reg_fpscr: name = "fpscr"; break;
		case reg_sr_status: name = "sr"; break;
		case reg_pc_dyn: name = "pc_dyn"; break;
		case reg_macl: name = "macl"; break;
		case reg_mach: name = "mach"; break;
		case reg_pr: name = "pr"; break;
		case reg_gbr: name = "gbr"; break;
		case reg_nextpc: name = "pc"; break;
		case reg_fpul: name = "fpul"; break;
		case reg_old_fpscr: name = "old_fpscr"; break;
		case reg_old_sr_status: name = "old_sr_status"; break;
		case reg_ssr: name = "ssr"; break;
		default:
			sprintf(tmp, "s%d", (int)reg);
			break;
		}
	}
	if (size == 0)
		return;
	strncpy(buf, name != NULL ? name : tmp, size - 1);
	buf[size - 1] = '\0';
}

/* @prm as text after what is in @buf (", " before it where @comma) */
static void disasm_param(char *buf, size_t size, const shil_param *prm, bool comma)
{
	size_t at = strlen(buf);
	char tmp[96];
	tmp[0] = '\0';
	if (!shil_is_null(prm) && comma)
		strcpy(tmp, ", ");
	if (shil_is_imm(prm))
	{
		if (is_s8(prm->_imm))
			sprintf(tmp + strlen(tmp), "%d", (int)(s32)prm->_imm);
		else
			sprintf(tmp + strlen(tmp), "0x%x", prm->_imm);
	}
	else if (shil_is_reg(prm))
	{
		shil_name_reg(tmp + strlen(tmp), 48, shil_reg(prm));
		if (shil_count(prm) > 1)
			sprintf(tmp + strlen(tmp), "v%u", shil_count(prm));
		sprintf(tmp + strlen(tmp), ".%u", (unsigned)prm->version[0]);
	}
	if (at + 1 < size)
	{
		strncpy(buf + at, tmp, size - at - 1);
		buf[size - 1] = '\0';
	}
}

void shil_disasm(char *buf, size_t size, const shil_opcode *op)
{
	size_t at;
	if (size == 0)
		return;
	buf[0] = '\0';
	strncpy(buf, shilop_str[op->op], size - 1);
	buf[size - 1] = '\0';
	at = strlen(buf);
	if (at + 1 < size)
	{
		buf[at] = ' ';
		buf[at + 1] = '\0';
	}
	disasm_param(buf, size, &op->rd, false);
	disasm_param(buf, size, &op->rd2, true);
	at = strlen(buf);
	if (at + 4 < size)
		strcpy(buf + at, " <- ");
	disasm_param(buf, size, &op->rs1, false);
	disasm_param(buf, size, &op->rs2, true);
	disasm_param(buf, size, &op->rs3, true);
}

const char* shil_opcode_name(int op)
{
	return shilop_str[op];
}
