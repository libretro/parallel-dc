/* The sound chip's ARM7 recompiled on a 32-bit ARM host, on
 * jit/a32_emit.h: an instruction that only works on registers and the flags
 * is run as it is, on host registers it is given, and the rest are calls.
 * (The other processors have arm7_rec.c's.) C89. */
#include <stdlib.h>
#include <string.h>
#include "arm7_interp.h"
#include "hw/mem/_vmem.h"

#if FEAT_AREC != DYNAREC_NONE && !(HOST_CPU == CPU_X64 || HOST_CPU == CPU_X86 || HOST_CPU == CPU_ARM64)

#if HOST_CPU != CPU_ARM
#error "no ARM7 recompiler for this processor: build it with FEAT_AREC=DYNAREC_NONE"
#endif

#include <sys/mman.h>

void CompileCode(void);
void arm_dispatch(void);
void arm_exit(void);
void DYNACALL arm_mainloop(u32 cycl, void* regs, void* entrypoints);
void DYNACALL arm_compilecode(void);

/* Emulate a single arm op, passed in opcode */
u32 DYNACALL arm_single_op(u32 opcode)
{
	u32 clockTicks = 0;

#define NO_OPCODE_READ
#include "arm-new.h"
#undef NO_OPCODE_READ

	return clockTicks;
}

/*
	ARM
		ALU opcodes (more or less)

			(flags,rv)=opcode(flags,in regs ..)
			rd=rv;
			if (set_flags)
				PSR=(rd==pc?CPSR:flags);

		(mem ops)
		Writes of R15:
			R15+12
		R15 as base:
			R15+8
		LDR
			rd=mem[addr(in regs)]
		LDM

		...
		STR/STM: pc+12

		"cached" interpreter:
		Set PC+12 to PC reg
		mov opcode
		call function

		if (pc settting opcode)
			lookup again using armNextPC

		PC setting opcodes
			ALU with write to PC
			LDR with write to PC (SDT)
			LDM with write to PC (BDT)
			B/BL
			SWI
			<Undefined opcodes, if any>

			Indirect, via write to PSR/Mode
			MSR
*/

typedef struct ArmDPOP
{
	u32 key;
	u32 mask;
	u32 flags;
} ArmDPOP;

/* (16 opcodes, three forms each) */
static ArmDPOP ops[16 * 3];
static u32 op_count;

enum OpFlags
{
	OP_SETS_PC         = 1,
	OP_READS_PC        = 32768,
	OP_IS_COND         = 65536,
	OP_ALIGNED         = 131072, /* a load from a whole word's address */
	OP_MFB             = (int)0x80000000,

	OP_HAS_RD_12       = 2,
	OP_HAS_RD_16       = 4,
	OP_HAS_RS_0        = 8,
	OP_HAS_RS_8        = 16,
	OP_HAS_RS_16       = 32,
	OP_HAS_FLAGS_READ  = 4096,
	OP_HAS_FLAGS_WRITE = 8192,
	OP_HAS_RD_READ     = 16384, /* For conditionals */

	OP_WRITE_FLAGS     = 64,
	OP_WRITE_FLAGS_S   = 128,
	OP_READ_FLAGS      = 256,
	OP_READ_FLAGS_S    = 512,
	OP_WRITE_REG       = 1024,
	OP_READ_REG_1      = 2048
};

#define DP_R_ROFC (OP_READ_FLAGS_S|OP_READ_REG_1) /* Reads reg1, op2, flags if S */
#define DP_R_ROF (OP_READ_FLAGS|OP_READ_REG_1)    /* Reads reg1, op2, flags (ADC & co) */
#define DP_R_OFC (OP_READ_FLAGS_S)                /* Reads op2, flags if S */

#define DP_W_RFC (OP_WRITE_FLAGS_S|OP_WRITE_REG)  /* Writes reg, and flags if S */
#define DP_W_F (OP_WRITE_FLAGS)                   /* Writes only flags, always (S=1) */

/*
	COND | 00 0 OP1   S Rn Rd SA    ST 0  Rm -- Data opcode, PSR xfer (imm shifted reg)
	     | 00 0 OP1   S Rn Rd Rs   0 ST 1 Rm -- Data opcode, PSR xfer (reg shifted reg)
	     | 00 0 0 00A S Rd Rn Rs    1001  Rm -- Mult
	     | 00 0 1 0B0 0 Rn Rd 0000  1001  Rm -- SWP
	     | 00 1 OP1   S Rn Rd imm8r4         -- Data opcode, PSR xfer (imm8r4)

	     | 01 0 P UBW L Rn Rd Offset          -- LDR/STR (I=0)
	     | 01 1 P UBW L Rn Rd SHAM SHTP 0 Rs  -- LDR/STR (I=1)
	     | 10 0 P USW L Rn {RList}            -- LDM/STM
	     | 10 1 L {offset}                    -- B/BL
	     | 11 1 1 X*                          -- SWI

	(undef cases)
	     | 01 1 XXXX X X*  X*  X* 1 XXXX - Undefined (LDR/STR w/ encodings that would be reg. based shift)
	     | 11 0 PUNW L Rn {undef} -- Copr. Data xfer (undef)
	     | 11 1 0 CPOP Crn Crd Cpn CP3 0 Crm -- Copr. Data Op (undef)
	     | 11 1 0 CPO3 L Crn Crd Cpn CP3 1 Crm -- Copr. Reg xf (undef)
*/

static void AddDPOP(u32 subcd, u32 rflags, u32 wflags)
{
	ArmDPOP op;
	u32 key = subcd << 21;
	u32 mask = (15 << 21) | (7 << 25);

	op.flags = rflags | wflags;

	if (wflags == DP_W_F)
	{
		/* also match S bit for opcodes that must write to flags (CMP & co) */
		mask |= 1 << 20;
		key |= 1 << 20;
	}

	/* ISR form (bit 25=0, bit 4 = 0) */
	op.key = key;
	op.mask = mask | (1 << 4);
	ops[op_count++] = op;

	/* RSR form (bit 25=0, bit 4 = 1, bit 7=0) */
	op.key = key | (1 << 4);
	op.mask = mask | (1 << 4) | (1 << 7);
	ops[op_count++] = op;

	/* imm8r4 form (bit 25=1) */
	op.key = key | (1 << 25);
	op.mask = mask;
	ops[op_count++] = op;
}

static void InitHash(void)
{
	op_count = 0;

	/* AND, EOR, SUB, RSB, ADD, ORR, BIC: Rn, OPER2, {Flags} -> Rd, {Flags} */
	AddDPOP(0, DP_R_ROFC, DP_W_RFC);
	AddDPOP(1, DP_R_ROFC, DP_W_RFC);
	AddDPOP(2, DP_R_ROFC, DP_W_RFC);
	AddDPOP(3, DP_R_ROFC, DP_W_RFC);
	AddDPOP(4, DP_R_ROFC, DP_W_RFC);
	AddDPOP(12, DP_R_ROFC, DP_W_RFC);
	AddDPOP(14, DP_R_ROFC, DP_W_RFC);

	/* ADC, SBC, RSC: Rn, OPER2, Flags -> Rd, {Flags} */
	AddDPOP(5, DP_R_ROF, DP_W_RFC);
	AddDPOP(6, DP_R_ROF, DP_W_RFC);
	AddDPOP(7, DP_R_ROF, DP_W_RFC);

	/* TST, TEQ (S=1): Rn, OPER2, Flags -> Flags */
	AddDPOP(8, DP_R_ROF, DP_W_F);
	AddDPOP(9, DP_R_ROF, DP_W_F);

	/* CMP, CMN (S=1): Rn, OPER2 -> Flags */
	AddDPOP(10, DP_R_ROF, DP_W_F);
	AddDPOP(11, DP_R_ROF, DP_W_F);

	/* MOV, MVN: OPER2, {Flags} -> Rd, {Flags} */
	AddDPOP(13, DP_R_OFC, DP_W_RFC);
	AddDPOP(15, DP_R_OFC, DP_W_RFC);
}

static void armEmit32(u32 emit32);
static void *armGetEmitPtr(void);

#define A32_EMIT(w) armEmit32(w)
#define A32_HERE() armGetEmitPtr()
#include "jit/a32_emit.h"

static u8* icPtr;
static u8* ICache;

#define ICacheSize (1024 * 1024)
#if defined(_WIN32)
u8 ARM7_TCB[ICacheSize + 4096];
#elif defined(__linux__) || defined(HAVE_LIBNX)
u8 ARM7_TCB[ICacheSize + 4096] __attribute__((section(".text")));
#elif defined(__APPLE__)
u8 ARM7_TCB[ICacheSize + 4096] __attribute__((section("__TEXT, .text")));
#else
#error ARM7_TCB ALLOC
#endif

void* EntryPoints[ARAM_SIZE_MAX / 4];

/* Every block there is: where it starts and how many words of the program
 * it was made from. For FlushCache(), which puts back only the entry points
 * that were given out, and for arm7_rec_same_code(). A block is never
 * under 16 bytes of code: there is room for one for every 16 bytes of the
 * code buffer, and so for all there can be before it is full. */
typedef struct CompiledBlock
{
	u32 pc;
	u32 words;
} CompiledBlock;
#define MAX_BLOCKS (ICacheSize / 16)
static CompiledBlock *compiled_blocks;
static u32 compiled_block_count;
/* whether the entry points have all been set once */
static bool entry_points_filled;

typedef enum OpType
{
	VOT_Fallback,
	VOT_DataOp,
	VOT_B,
	VOT_BL,
	VOT_BR,     /* Branch (to register) */
	VOT_Read,   /* Actually, this handles LDR and STR */
	VOT_MRS,
	VOT_MSR
} OpType;

static u32 DYNACALL DoMemOp_load8(u32 addr, u32 data) { (void)data; return arm_ReadMem8(addr); }
static u32 DYNACALL DoMemOp_load32(u32 addr, u32 data) { (void)data; return arm_ReadMem32(addr); }
static u32 DYNACALL DoMemOp_store8(u32 addr, u32 data) { arm_WriteMem8(addr, data); return 0; }
static u32 DYNACALL DoMemOp_store32(u32 addr, u32 data) { arm_WriteMem32(addr, data); return 0; }

/* findfirstset -- used in LDM/STM handling */
#define findfirstset __builtin_ffs

static uintptr_t GetMemOp(bool Load, bool Byte)
{
	if (Load)
		return Byte ? (uintptr_t)&DoMemOp_load8 : (uintptr_t)&DoMemOp_load32;
	return Byte ? (uintptr_t)&DoMemOp_store8 : (uintptr_t)&DoMemOp_store32;
}

/* Decodes an opcode, returns type. opcd might be changed (currently for
 * LDM/STM -> LDR/STR transforms) */
static OpType DecodeOpcode(u32 *popcd, u32 *pflags)
{
	u32 opcd = *popcd;
	u32 flags;
	u32 CC = (opcd >> 28);
	bool _set_pc;
	u32 RList, Rn, i;

	/* by default, PC has to be updated */
	flags = OP_READS_PC;

	if (CC != A32_AL)
		flags |= OP_IS_COND;

	/* helpers ... */
#define CHK_BTS(M,S,V) ( (M & (opcd>>S)) == (V) ) /* Check bits value in opcode */
#define IS_LOAD (opcd & (1<<20))                  /* Is L bit set ? (LDM/STM LDR/STR) */
#define READ_PC_CHECK(S) if (CHK_BTS(15,S,15)) flags|=OP_READS_PC;

	/* Opcode sets pc ? */
	_set_pc =
		(CHK_BTS(3,26,0) && CHK_BTS(15,12,15))             || /* Data processing w/ Rd=PC */
		(CHK_BTS(3,26,1) && CHK_BTS(15,12,15) && IS_LOAD ) || /* LDR/STR w/ Rd=PC */
		(CHK_BTS(7,25,4) && (opcd & 32768) &&  IS_LOAD)    || /* LDM/STM w/ PC in list */
		CHK_BTS(7,25,5)                                    || /* B or BL */
		CHK_BTS(15,24,15);                                    /* SWI */

	/* NV condition means VFP on newer cores, let interpreter handle it... */
	if (CC == 15)
		goto out_fallback;

	if (_set_pc)
		flags |= OP_SETS_PC;

	/* B / BL ? */
	if (CHK_BTS(7,25,5))
	{
		verify(_set_pc);
		if (!(flags & OP_IS_COND))
			flags &= ~OP_READS_PC;  /* not COND doesn't read from pc */

		flags |= OP_SETS_PC;        /* Branches Set pc .. */

		*pflags = flags;
		return (opcd & (1 << 24)) ? VOT_BL : VOT_B;
	}

	/* Common case: MOVCC PC,REG */
	if (CHK_BTS(0xFFFFFF,4,0x1A0F00))
	{
		verify(_set_pc);
		if (CC == A32_AL)
			flags &= ~OP_READS_PC;

		*pflags = flags;
		return VOT_BR;
	}

	/* No support for COND branching opcodes apart from the forms above .. */
	if (CC != A32_AL && _set_pc)
		goto out_fallback;

	RList = opcd & 0xFFFF;
	Rn = (opcd >> 16) & 15;

#define LDM_REGCNT() (cpuBitsSet[RList & 255] + cpuBitsSet[(RList >> 8) & 255])

	/* Data Processing opcodes -- find using mask/key */
	for (i = 0; i < op_count; i++)
	{
		if (!_set_pc && ops[i].key == (opcd & ops[i].mask))
		{
			/* We fill in the cases that we have to read pc */
			flags &= ~OP_READS_PC;

			/* Conditionals always need flags read ... */
			if ((opcd >> 28) != 0xE)
			{
				flags |= OP_HAS_FLAGS_READ;
				flags |= OP_HAS_RD_READ;
			}

			if ((ops[i].flags & OP_READ_FLAGS) ||
			   ((ops[i].flags & OP_READ_FLAGS_S) && (opcd & (1 << 20))))
				flags |= OP_HAS_FLAGS_READ;

			if ((ops[i].flags & OP_WRITE_FLAGS) ||
			   ((ops[i].flags & OP_WRITE_FLAGS_S) && (opcd & (1 << 20))))
				flags |= OP_HAS_FLAGS_WRITE;

			if (ops[i].flags & OP_WRITE_REG)
			{
				/* All dpops that write, write to RD_12 */
				flags |= OP_HAS_RD_12;
				verify(! (CHK_BTS(15,12,15) && CC != A32_AL));
			}

			if (ops[i].flags & OP_READ_REG_1)
			{
				/* Reg 1 is RS_16 */
				flags |= OP_HAS_RS_16;

				/* reads from pc ? */
				READ_PC_CHECK(16);
			}

			/* op2 is imm or reg ? */
			if ( !(opcd & (1 << 25)) )
			{
				/* its reg (register or imm shifted) */
				flags |= OP_HAS_RS_0;
				/* reads from pc ? */
				READ_PC_CHECK(0);

				/* is it register shifted reg ? */
				if (opcd & (1 << 4))
				{
					verify(! (opcd & (1 << 7)) );	/* must be zero */
					flags |= OP_HAS_RS_8;
					/* can't be pc ... */
					verify(!CHK_BTS(15,8,15));
				}
				else
				{
					/* is it RRX ? */
					if ( ((opcd >> 4) & 7) == 6)
					{
						/* RRX needs flags to be read (even if the opcode doesn't) */
						flags |= OP_HAS_FLAGS_READ;
					}
				}
			}

			*pflags = flags;
			return VOT_DataOp;
		}
	}

	/* Lets try mem opcodes since its not data processing */

	/* LDR/STR: CCCC 01 0 P UBW L Rn Rd Offset -- (I=0) */
	if ((opcd >> 25) == (0xE4 / 2))
	{
		arm_printf("ARM: MEM %08X L/S:%d, AWB:%d!\n", opcd, (opcd >> 20) & 1, (opcd >> 21) & 1);
		*pflags = flags;
		return VOT_Read;
	}
	else if ((opcd >> 25) == (0xE6 / 2) && CHK_BTS(0x7,4,0))
	{
		/* I=1: Logical Left shift, only */
		arm_printf("ARM: MEM REG to Reg %08X\n", opcd);
		*pflags = flags;
		return VOT_Read;
	}
	/* LDM common case */
	else if ((opcd >> 25) == (0xE8 / 2) && CHK_BTS(1,22,0) && CHK_BTS(1,20,1) && LDM_REGCNT() == 1)
	{
		/* One register xfered: can be rewriten as normal mem opcode */
		const u32 old_opcd = opcd;
		u32 Rd;

		opcd = 0xE4000000;

		/* Imm offset */
		opcd |= 0 << 25;
		/* Post incr */
		opcd |= old_opcd & (1 << 24);
		/* Up/Dn */
		opcd |= old_opcd & (1 << 23);
		/* Word/Byte */
		opcd |= 0 << 22;
		/* Write back (must be 0 for PI) */
		opcd |= old_opcd & (1 << 21);
		/* Load */
		opcd |= old_opcd & (1 << 20);

		/* Rn */
		opcd |= Rn << 16;

		/* Rd */
		Rd = findfirstset(RList) - 1;
		opcd |= Rd << 12;

		/* Offset */
		opcd |= 4;

		/* Not indexed after the access and no write back: the base as it is */
		if (!(old_opcd & ((1 << 24) | (1 << 21))))
			opcd = (opcd | (1 << 24)) & ~0xfffu;

		arm_printf("ARM: MEM TFX R %08X\n", opcd);

		*popcd = opcd;
		/* a block load reads a whole word whatever the base's low bits */
		*pflags = flags | OP_ALIGNED;
		return VOT_Read;
	}
	/* STM common case */
	else if ((opcd >> 25) == (0xE8 / 2) && CHK_BTS(1,22,0) && CHK_BTS(1,20,0) && LDM_REGCNT() == 1)
	{
		const u32 old_opcd = opcd;
		u32 Rd;

		opcd = 0xE4000000;

		/* Imm offset */
		opcd |= 0 << 25;
		/* Pre/Post incr */
		opcd |= old_opcd & (1 << 24);
		/* Up/Dn */
		opcd |= old_opcd & (1 << 23);
		/* Word/Byte */
		opcd |= 0 << 22;
		/* Write back */
		opcd |= old_opcd & (1 << 21);
		/* Store/Load */
		opcd |= old_opcd & (1 << 20);

		/* Rn */
		opcd |= Rn << 16;

		/* Rd */
		Rd = findfirstset(RList) - 1;
		opcd |= Rd << 12;

		/* Offset */
		opcd |= 4;

		/* Not indexed after the access and no write back: the base as it is */
		if (!(old_opcd & ((1 << 24) | (1 << 21))))
			opcd = (opcd | (1 << 24)) & ~0xfffu;

		arm_printf("ARM: MEM TFX W %08X\n", opcd);

		*popcd = opcd;
		*pflags = flags;
		return VOT_Read;
	}
	else if (CHK_BTS(0xE10F0FFF,0,0xE10F0000))
	{
		*pflags = flags;
		return VOT_MRS;
	}
	else if (CHK_BTS(0xEFBFFFF0,0,0xE129F000))
	{
		*pflags = flags;
		return VOT_MSR;
	}
	else if ((opcd >> 25) == (0xE8 / 2) && CHK_BTS(32768,0,0))
	{
		arm_printf("ARM: MEM FB %08X\n", opcd);
		flags |= OP_MFB; /* (flag Just for the fallback counters) */
	}
	else
	{
		arm_printf("ARM: FB %08X\n", opcd);
	}

	/* by default fallback to interpr */
	*pflags = flags;
	return VOT_Fallback;

out_fallback:
	*pflags = flags;
	return VOT_Fallback;
}

/* helpers ... */
static void LoadReg(int rd, u32 regn, int cc)
{
	a32_ls_imm(A32_LDR, rd, A32_R8, (s32)((u8*)&reg[regn].I - (u8*)&reg[0].I), A32_OFFSET, cc);
}

static void StoreReg(int rd, u32 regn, int cc)
{
	a32_ls_imm(A32_STR, rd, A32_R8, (s32)((u8*)&reg[regn].I - (u8*)&reg[0].I), A32_OFFSET, cc);
}

static void armv_add_imm(int regd, int regn, s32 imm)
{
	if (imm >= 0)
		a32_op_imm(A32_ADD, regd, regn, (u32)imm, A32_AL);
	else
		a32_op_imm(A32_SUB, regd, regn, (u32)-imm, A32_AL);
}

/* very quick-and-dirty register rename based virtualisation */
static u32 renamed_regs[16];
static u32 rename_reg_base;

static void RenameRegReset(void)
{
	rename_reg_base = A32_R1;
	memset(renamed_regs, 0, sizeof(renamed_regs));
}

/* returns new reg #. didrn is true if a rename mapping was added */
static u32 RenameReg(u32 regn, bool *didrn)
{
	if (renamed_regs[regn] == 0)
	{
		renamed_regs[regn] = rename_reg_base;
		/* r1-r3, then r12 (r4, r5, r8, r9 are taken) */
		rename_reg_base = (rename_reg_base == A32_R3) ? A32_R12 : rename_reg_base + 1;
		*didrn = true;
	}
	else
		*didrn = false;

	return renamed_regs[regn];
}

/* For reg reads (they need to be loaded); load can be used to skip loading
 * (for RD if not cond) */
static void LoadAndRename(u32 *opcd, u32 bitpos, bool load, u32 pc)
{
	bool didrn;
	const u32 regn = (*opcd >> bitpos) & 15;
	const u32 nreg = RenameReg(regn, &didrn);

	*opcd = (*opcd & ~(15u << bitpos)) | (nreg << bitpos);

	if (load && didrn)
	{
		if (regn == 15)
			a32_mov32((int)nreg, pc, A32_AL);
		else
			LoadReg((int)nreg, regn, A32_AL);
	}
}

/* For results store (they need to be stored) */
static void StoreAndRename(u32 opcd, u32 bitpos)
{
	bool didrn;
	u32 regn = (opcd >> bitpos) & 15;
	const u32 nreg = RenameReg(regn, &didrn);

	verify(!didrn);

	if (regn == 15)
		regn = R15_ARM_NEXT;

	StoreReg((int)nreg, regn, A32_AL);
}

/* For COND */
static void LoadFlags(void)
{
	/* Load flags, and move them to the flags register */
	LoadReg(A32_R0, RN_PSR_FLAGS, A32_AL);
	a32_msr(0, 8, A32_R0, A32_AL);
}

static void StoreFlags(void)
{
	/* get results from flags register, and store them */
	a32_mrs(A32_R1, 0, A32_AL);
	StoreReg(A32_R1, RN_PSR_FLAGS, A32_AL);
}

/* Virtualise Data Processing opcode */
static void VirtualizeOpcode(u32 opcd, u32 flag, u32 pc)
{
	/* Keep original opcode for info */
	const u32 orig = opcd;
	/* Dynamic LSL/LSR/ASR/ROR adds +4 to pc due to delay */
	const bool shiftByReg = !(opcd & (1 << 25)) && (opcd & (1 << 4));

	/* Load arm flags, RS0/8/16, RD12/16 (as indicated by the decoder flags) */
	if (flag & OP_HAS_FLAGS_READ)
		LoadFlags();

	if (flag & OP_HAS_RS_0)
		LoadAndRename(&opcd, 0, true, pc + (shiftByReg ? 12 : 8));
	if (flag & OP_HAS_RS_8)
		LoadAndRename(&opcd, 8, true, pc + 8);
	if (flag & OP_HAS_RS_16)
		LoadAndRename(&opcd, 16, true, pc + (shiftByReg ? 12 : 8));

	if (flag & OP_HAS_RD_12)
		LoadAndRename(&opcd, 12, (flag & OP_HAS_RD_READ) != 0, pc + 4);

	if (flag & OP_HAS_RD_16)
	{
		verify(! (flag & OP_HAS_RS_16));
		LoadAndRename(&opcd, 16, (flag & OP_HAS_RD_READ) != 0, pc + 4);
	}

	/* Fields that should be zero are, whatever the guest put there */
	if (!(flag & (OP_HAS_RS_16 | OP_HAS_RD_16)))
		opcd &= ~(15u << 16);
	if (!(flag & OP_HAS_RD_12))
		opcd &= ~(15u << 12);

	/* Opcode has been modified to use the new regs: emit it */
	arm_printf("Arm Virtual: %08X -> %08X\n", orig, opcd);
	armEmit32(opcd);

	/* Store arm flags, rd12/rd16 (as indicated by the decoder flags) */
	if (flag & OP_HAS_RD_12)
		StoreAndRename(orig, 12);

	if (flag & OP_HAS_RD_16)
		StoreAndRename(orig, 16);

	/* Sanity check */
	if (renamed_regs[15] != 0)
		verify(flag & OP_READS_PC || (flag & OP_SETS_PC && !(flag & OP_IS_COND)));

	if (flag & OP_HAS_FLAGS_WRITE)
		StoreFlags();
}

static void *armGetEmitPtr(void)
{
	if (icPtr < (ICache + ICacheSize - 1024))
		return (void *)icPtr;
	return NULL;
}

static void armEmit32(u32 emit32)
{
	if (icPtr >= (ICache + ICacheSize - 1024))
		die("ICache is full, invalidate old entries ...");

	memcpy(icPtr, &emit32, 4);
	icPtr += 4;
}

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
void armFlushICache(void *code, void *pEnd)
{
	sys_dcache_flush(code, (u8*)pEnd - (u8*)code + 1);
	sys_icache_invalidate(code, (u8*)pEnd - (u8*)code + 1);
}
#else
void armFlushICache(void *bgn, void *end)
{
	__builtin___clear_cache((char *)bgn, (char *)end);
}
#endif

static void armv_imm_to_reg(u32 regn, u32 imm)
{
	a32_mov32(A32_R0, imm, A32_AL);
	StoreReg(A32_R0, regn, A32_AL);
}

static void armv_intpr(u32 opcd)
{
	/* Call interpreter */
	a32_mov32(A32_R0, opcd, A32_AL);
	a32_call((uintptr_t)arm_single_op, A32_AL);
	/* take off the cycles it returns */
	a32_op(A32_SUB, A32_R5, A32_R5, A32_R0, A32_AL);
}

static void armv_end(void* codestart, u32 cycl)
{
	/* Normal block end: cycle counter rv */
	if (a32_is_imm(cycl))
		a32_dp_imm(A32_SUB, true, A32_R5, A32_R5, cycl, A32_AL);
	else
	{
		u32 togo = cycl;
		while (!a32_is_imm(togo))
		{
			a32_op_imm(A32_SUB, A32_R5, A32_R5, 256, A32_AL);
			togo -= 256;
		}
		a32_dp_imm(A32_SUB, true, A32_R5, A32_R5, togo, A32_AL);
	}
	a32_jump((uintptr_t)&arm_exit, A32_MI);	/* statically predicted as not taken */
	a32_jump((uintptr_t)&arm_dispatch, A32_AL);

	armFlushICache(codestart, armGetEmitPtr());
}

static void FlushCache(void);

/* Run a timeslice for ARMREC */
void aicaarm_run(u32 samples)
{
	/* The interpreter instead, when the core option asks for it (as in
	 * arm7_rec.c); coming back, everything compiled is thrown away */
	static bool interpreting;
	u32 i;

	if (settings.dynarec.Arm7Interpreter)
	{
		interpreting = true;
		for (i = 0; i < samples; i++)
		{
			arm_Run_(ARM_CYCLES_PER_SAMPLE);
			libAICA_TimeStep();
		}
		return;
	}
	if (interpreting)
	{
		interpreting = false;
		FlushCache();
	}

	for (i = 0; i < samples; i++)
	{
		if (Arm7Enabled)
			arm_mainloop(ARM_CYCLES_PER_SAMPLE, arm_Reg, EntryPoints);
		libAICA_TimeStep();
	}
}

/* Mem operand 2 calculation, if Reg or large imm */
static void MemOperand2(int dst, bool I, bool U, u32 offs, u32 opcd)
{
	if (I)
	{
		const u32 Rm = (opcd >> 0) & 15;
		u32 SA;
		verify(CHK_BTS(7,4,0));	/* only SHL mode */
		LoadReg(A32_R1, Rm, A32_AL);
		SA = 31 & (opcd >> 7);
		/* can't do shifted add for now -- EMITTER LIMIT -- */
		if (SA)
			a32_shift_imm(A32_LSL, false, A32_R1, A32_R1, SA, A32_AL);
	}
	else
		a32_mov32(A32_R1, offs, A32_AL);

	if (U)
		a32_op(A32_ADD, dst, A32_R0, A32_R1, A32_AL);
	else
		a32_op(A32_SUB, dst, A32_R0, A32_R1, A32_AL);
}

static void DYNACALL MSR_do_spsr(u32 v)
{
	if (armMode > 0x10 && armMode < 0x1f) /* !=0x10 ? */
		reg[17].I = (reg[17].I & 0x00FFFF00) | (v & 0xFF0000FF);
}

static void DYNACALL MSR_do_cpsr(u32 v)
{
	u32 newValue;

	CPUUpdateCPSR();

	newValue = reg[16].I;
	if (armMode > 0x10)
		newValue = (newValue & 0xFFFFFF00) | (v & 0x000000FF);

	newValue = (newValue & 0x00FFFFFF) | (v & 0xFF000000);
	newValue |= 0x10;
	if (armMode > 0x10)
		CPUSwitchMode(newValue & 0x1f, false);
	reg[16].I = newValue;
	CPUUpdateFlags();
}

/* Compile & run block of code, starting armNextPC */
void CompileCode(void)
{
	/* Get the code ptr */
	void* rv = armGetEmitPtr();
	/* setup local pc counter (of whole words: the block is found by its
	 * address without the low two bits, and has to be the code for that
	 * address) */
	u32 pc = armNextPC & ~3u;
	/* the ops counter is used to terminate the block: we don't want too
	 * long blocks for timing accuracy */
	u32 nops = 0;
	u32 Cycles = 0;

	/* update the block table. Note that we mask with the max aica size (8
	 * MB), which is also the size of the EntryPoints table. This way the
	 * dynarec main loop doesn't have to worry about the actual aica ram
	 * size. The aica ram always wraps to 8 MB anyway. */
	EntryPoints[(armNextPC & (ARAM_SIZE_MAX - 1)) / 4] = rv;

	for (;;)
	{
		u32 opcd, op_flags;
		OpType opt;

		nops++;

		/* Each opcode takes at least 6 cycles */
		Cycles += 6;

		/* Read opcode ... */
		opcd = CPUReadMemoryQuick(pc);

		/* Decode & handle opcode */
		opt = DecodeOpcode(&opcd, &op_flags);

		switch (opt)
		{
		case VOT_DataOp:
			/* data processing opcode that can be virtualised */
			RenameRegReset();
			VirtualizeOpcode(opcd, op_flags, pc);
			break;

		case VOT_BR:
			{
				/* Branch to reg */
				const int cc = (int)(opcd >> 28);

				verify(op_flags & OP_SETS_PC);

				if (cc != A32_AL)
				{
					LoadFlags();
					armv_imm_to_reg(R15_ARM_NEXT, pc + 4);
				}

				LoadReg(A32_R0, opcd & 0xF, A32_AL);
				a32_op_imm(A32_BIC, A32_R0, A32_R0, 3, A32_AL);
				StoreReg(A32_R0, R15_ARM_NEXT, cc);
				Cycles += 3;
			}
			break;

		case VOT_B:
		case VOT_BL:
			{
				/* Branch to imm: <<2, sign extend ! */
				const s32 offs = ((s32)opcd << 8) >> 6;

				if (op_flags & OP_IS_COND)
				{
					const int cc = (int)(opcd >> 28);
					armv_imm_to_reg(R15_ARM_NEXT, pc + 4);
					LoadFlags();
					if (opt == VOT_BL)
					{
						a32_mov32(A32_R0, pc + 4, A32_AL);
						StoreReg(A32_R0, 14, cc);
					}

					a32_mov32(A32_R0, pc + 8 + offs, A32_AL);
					StoreReg(A32_R0, R15_ARM_NEXT, cc);
				}
				else
				{
					if (opt == VOT_BL)
						armv_imm_to_reg(14, pc + 4);

					armv_imm_to_reg(R15_ARM_NEXT, pc + 8 + offs);
				}
				Cycles += 3;
			}
			break;

		case VOT_Read:
			{
				/* LDR/STR */
				const u32 offs = opcd & 4095;
				const bool U = (opcd & (1 << 23)) != 0;
				const bool Pre = (opcd & (1 << 24)) != 0;
				const bool W = (opcd & (1 << 21)) != 0;
				const bool I = (opcd & (1 << 25)) != 0;
				const bool L = (opcd & (1 << 20)) != 0;
				const u32 Rn = (opcd >> 16) & 15;
				const u32 Rd = (opcd >> 12) & 15;
				/* Write back if pre- or post-indexed and Rn!=Rd */
				bool DoWB = (W || !Pre) && Rn != Rd;
				bool DoAdd = DoWB || Pre;

				/* Register not updated anyway */
				if (!I && offs == 0)
				{
					DoWB = false;
					DoAdd = false;
				}

				verify(!((Rn == 15) && DoWB));

				/* AGU */
				if (Rn != 15)
				{
					LoadReg(A32_R0, Rn, A32_AL);

					if (DoAdd)
					{
						const int dst = Pre ? A32_R0 : A32_R9;

						if (!I && a32_is_imm(offs))
						{
							if (U)
								armv_add_imm(dst, A32_R0, (s32)offs);
							else
								armv_add_imm(dst, A32_R0, -(s32)offs);
						}
						else
							MemOperand2(dst, I, U, offs, opcd);

						if (DoWB && dst == A32_R0)
							a32_mov(A32_R9, A32_R0, A32_AL);
					}
				}
				else
				{
					u32 addr = pc + 8;

					if (Pre && offs && !I)
						addr += U ? offs : -offs;

					a32_mov32(A32_R0, addr, A32_AL);

					if (Pre && I)
					{
						MemOperand2(A32_R1, I, U, offs, opcd);
						a32_op(A32_ADD, A32_R0, A32_R0, A32_R1, A32_AL);
					}
				}

				if (L && (op_flags & OP_ALIGNED))
					a32_op_imm(A32_BIC, A32_R0, A32_R0, 3, A32_AL);

				if (!L)
				{
					if (Rd == 15)
						a32_mov32(A32_R1, pc + 12, A32_AL);
					else
						LoadReg(A32_R1, Rd, A32_AL);
				}
				/* Call handler */
				a32_call(GetMemOp(L, CHK_BTS(1,22,1)), A32_AL);

				if (L)
				{
					if (Rd == 15)
					{
						verify(op_flags & OP_SETS_PC);
						/* the program counter has no low two bits */
						a32_op_imm(A32_BIC, A32_R0, A32_R0, 3, A32_AL);
						StoreReg(A32_R0, R15_ARM_NEXT, A32_AL);
					}
					else
						StoreReg(A32_R0, Rd, A32_AL);
				}

				/* Write back from AGU, if any */
				if (DoWB)
					StoreReg(A32_R9, Rn, A32_AL);
				if (L)
					Cycles += 4;
				else
					Cycles += 3;
			}
			break;

		case VOT_MRS:
			{
				const u32 Rd = (opcd >> 12) & 15;

				a32_call((uintptr_t)&CPUUpdateCPSR, A32_AL);

				if (opcd & (1 << 22))
					LoadReg(A32_R0, 17, A32_AL);
				else
					LoadReg(A32_R0, 16, A32_AL);

				StoreReg(A32_R0, Rd, A32_AL);
			}
			break;

		case VOT_MSR:
			{
				const u32 Rm = (opcd >> 0) & 15;

				LoadReg(A32_R0, Rm, A32_AL);
				if (opcd & (1 << 22))
					a32_call((uintptr_t)&MSR_do_spsr, A32_AL);
				else
					a32_call((uintptr_t)&MSR_do_cpsr, A32_AL);

				if (op_flags & OP_SETS_PC)
					armv_imm_to_reg(R15_ARM_NEXT, pc + 4);
				Cycles++;
			}
			break;

		case VOT_Fallback:
			/* interpreter fallback: let the interpreter count cycles */
			Cycles -= 6;

			/* arm_single_op needs PC+4 on r15 */
			armv_imm_to_reg(15, pc + 8);

			/* For cond branch, MSR */
			if (op_flags & OP_SETS_PC)
				armv_imm_to_reg(R15_ARM_NEXT, pc + 4);

			armv_intpr(opcd);
			break;

		default:
			die("can't happen\n");
		}

		/* Branch ? */
		if (op_flags & OP_SETS_PC)
		{
			arm_printf("ARM: %06X: Block End %d\n", pc, nops);
			break;
		}

		/* block size limit ? */
		if (nops > 32)
		{
			arm_printf("ARM: %06X: Block split %d\n", pc, nops);

			armv_imm_to_reg(R15_ARM_NEXT, pc + 4);
			break;
		}

		/* Goto next opcode */
		pc += 4;
	}

	{
		/* (the loop is left at the last word it read) */
		const u32 start = armNextPC & ~3u;
		if (compiled_block_count < MAX_BLOCKS)
		{
			compiled_blocks[compiled_block_count].pc = start;
			compiled_blocks[compiled_block_count].words = (pc - start) / 4 + 1;
			compiled_block_count++;
		}
	}

	armv_end(rv, Cycles);
}

static void FlushCache(void)
{
	u32 i;
	icPtr = ICache;
	/* There are two million entry points, 8 or 16 megabytes of them, and a
	 * few hundred blocks: only the ones that were given out are put back.
	 * All of them the first time. */
	if (!entry_points_filled)
	{
		for (i = 0; i < sizeof(EntryPoints) / sizeof(EntryPoints[0]); i++)
			EntryPoints[i] = (void*)(uintptr_t)&arm_compilecode;
		entry_points_filled = true;
	}
	else
	{
		for (i = 0; i < compiled_block_count; i++)
			EntryPoints[(compiled_blocks[i].pc & (ARAM_SIZE_MAX - 1)) / 4] = (void*)(uintptr_t)&arm_compilecode;
	}
	compiled_block_count = 0;
}

/* Whether everything that has been compiled was compiled from what @aram -
 * the sound memory of a save state about to be loaded - has in the same
 * places: then it is all still good after the load. */
bool arm7_rec_same_code(const u8 *aram)
{
	u32 b, i;
	for (b = 0; b < compiled_block_count; b++)
		for (i = 0; i < compiled_blocks[b].words; i++)
		{
			const u32 at = (compiled_blocks[b].pc + i * 4) & ARAM_MASK & ~3u;

			if (memcmp(&aica_ram.data[at], aram + at, 4) != 0)
				return false;
		}
	return true;
}

static void armt_init(void)
{
	InitHash();

	/* align to next page */
	ICache = (u8*)(((uintptr_t)ARM7_TCB + 4095) & ~(uintptr_t)4095);

#ifdef __MACH__
	/* Can't just mprotect on iOS */
	munmap(ICache, ICacheSize);
	ICache = (u8*)mmap(ICache, ICacheSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE | MAP_ANON, 0, 0);
#endif

	mem_region_set_exec(ICache, ICacheSize);

#if TARGET_IPHONE
	memset((u8*)mmap(ICache, ICacheSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE | MAP_ANON, 0, 0), 0xFF, ICacheSize);
#else
	memset(ICache, 0xFF, ICacheSize);
#endif

	icPtr = ICache;

	if (compiled_blocks == NULL)
	{
		compiled_blocks = (CompiledBlock *)malloc(MAX_BLOCKS * sizeof(*compiled_blocks));
		verify(compiled_blocks != NULL);
	}
}

/* For the rest of the core (arm7.h) */
void arm7_rec_init(void)
{
	armt_init();
}

void arm7_rec_flush(void)
{
	FlushCache();
}

#else
/* (for the other processors: C has no empty file) */
typedef int arm7_rec_arm32_not_here;
#endif
