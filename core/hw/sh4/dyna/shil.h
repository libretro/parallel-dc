#ifndef SHIL_H
#define SHIL_H
/* The recompilers' intermediate operations (shil): C and C++. C++ has the
 * operands' and operations' methods as well (the recompilers' backends). */
#include <string.h>
#include "hw/sh4/sh4_if.h"

#ifdef __cplusplus
#include <new>
#include <string>
#endif

typedef struct shil_opcode shil_opcode;
typedef void shil_chfp(shil_opcode* op);
#ifdef __cplusplus
extern "C" {
#endif
extern shil_chfp* shil_chf[];
#ifdef __cplusplus
}
#endif

enum shil_param_type
{
	/* 2 bits */
	FMT_NULL,
	FMT_IMM,
	FMT_I32,
	FMT_F32,
	FMT_F64,
	
	FMT_V2,
	FMT_V3,
	FMT_V4,
	FMT_V8,
	FMT_V16,

	FMT_REG_BASE=FMT_I32,
	FMT_VECTOR_BASE=FMT_V2,

	FMT_MASK=0xFFFF
};

/*
	formats : 16u 16s 32u 32s, 32f, 64f
	param types: r32, r64
*/


#include "shilop.h"

typedef struct shil_param
{
#ifdef __cplusplus

	shil_param()
	{
		type=FMT_NULL;
		_imm=0xFFFFFFFF;
		memset(version, 0, sizeof(version));
	}
	shil_param(u32 type,u32 imm)
	{
		this->type=type;
		if (type >= FMT_REG_BASE)
			new (this) shil_param((Sh4RegType)imm);
		_imm=imm;
		memset(version, 0, sizeof(version));
	}

	shil_param(Sh4RegType reg)
	{
		type=FMT_NULL;
		if (reg>=reg_fr_0 && reg<=reg_xf_15)
		{
			type=FMT_F32;
			_imm=reg;
		}
		else if (reg>=regv_dr_0 && reg<=regv_dr_14)
		{
			type=FMT_F64;
			_imm=(reg-regv_dr_0)*2+reg_fr_0;
		}
		else if (reg>=regv_xd_0 && reg<=regv_xd_14)
		{
			type=FMT_F64;
			_imm=(reg-regv_xd_0)*2+reg_xf_0;
		}
		else if (reg>=regv_fv_0 && reg<=regv_fv_12)
		{
			type=FMT_V4;
			_imm=(reg-regv_fv_0)*4+reg_fr_0;
		}
		else if (reg==regv_xmtrx)
		{
			type=FMT_V16;
			_imm=reg_xf_0;
		}
		else if (reg==regv_fmtrx)
		{
			type=FMT_V16;
			_imm=reg_fr_0;
		}
		else
		{
			type=FMT_I32;
			_reg=reg;
		}
		memset(version, 0, sizeof(version));
	}
	union
	{
		u32 _imm;
		Sh4RegType _reg;
	};
#else
	u32 _imm;	/* (or the register, a Sh4RegType: one word either way) */
#endif

	u32 type;
	u16 version[16];

#ifdef __cplusplus
	bool is_null() const { return type==FMT_NULL; }
	bool is_imm() const { return type==FMT_IMM; }
	bool is_reg() const { return type>=FMT_REG_BASE; }

	bool is_r32i() const { return type==FMT_I32; }
	bool is_r32f() const { return type==FMT_F32; }
	u32 is_r32fv()  const { return type>=FMT_VECTOR_BASE?count():0; }
	bool is_r64f() const { return type==FMT_F64; }

	bool is_r32() const { return is_r32i() || is_r32f(); }
	bool is_r64() const { return is_r64f(); }	/* just here for symmetry ... */

	bool is_imm_s8() const { return is_imm() && is_s8(_imm); }
	bool is_imm_u8() const { return is_imm() && is_u8(_imm); }
	bool is_imm_s16() const { return is_imm() && is_s16(_imm); }
	bool is_imm_u16() const { return is_imm() && is_u16(_imm); }

	u32* reg_ptr()  const { verify(is_reg()); return GetRegPtr(_reg); }
	s32  reg_nofs()  const { verify(is_reg()); return (s32)((u8*)GetRegPtr(_reg) - (u8*)GetRegPtr(reg_xf_0)-sizeof(Sh4cntx)); }
	u32  reg_aofs()  const { return -reg_nofs(); }

	u32 imm_value() const { verify(is_imm()); return _imm; }

	bool is_vector() const { return type>=FMT_VECTOR_BASE; }

	u32 count() const { return  type==FMT_F64?2:type==FMT_V2?2:
								type==FMT_V3?3:type==FMT_V4?4:type==FMT_V8?8:
								type==FMT_V16?16:1; }	/* count of hardware regs */

	/*	
		Imms:
		is_imm
		
		regs:
		integer regs            : is_r32i,is_r32,count=1
		fpu regs, single view   : is_r32f,is_r32,count=1
		fpu regs, double view   : is_r64f,count=2
		fpu regs, quad view     : is_vector,is_r32fv=4, count=4
		fpu regs, matrix view   : is_vector,is_r32fv=16, count=16
	*/
#endif
} shil_param;

/* C's constructors: none, a constant or a register of a type (by
 * shil_param_type), and a register by its number (any view of it) */
static TYPES_INLINE shil_param shil_param_none(void)
{
	shil_param p;
	memset(&p, 0, sizeof(p));
	p.type = FMT_NULL;
	p._imm = 0xFFFFFFFF;
	return p;
}

static TYPES_INLINE shil_param shil_param_reg(u32 reg)
{
	shil_param p = shil_param_none();
	if (reg >= reg_fr_0 && reg <= reg_xf_15)
	{
		p.type = FMT_F32;
		p._imm = reg;
	}
	else if (reg >= regv_dr_0 && reg <= regv_dr_14)
	{
		p.type = FMT_F64;
		p._imm = (reg - regv_dr_0) * 2 + reg_fr_0;
	}
	else if (reg >= regv_xd_0 && reg <= regv_xd_14)
	{
		p.type = FMT_F64;
		p._imm = (reg - regv_xd_0) * 2 + reg_xf_0;
	}
	else if (reg >= regv_fv_0 && reg <= regv_fv_12)
	{
		p.type = FMT_V4;
		p._imm = (reg - regv_fv_0) * 4 + reg_fr_0;
	}
	else if (reg == regv_xmtrx)
	{
		p.type = FMT_V16;
		p._imm = reg_xf_0;
	}
	else if (reg == regv_fmtrx)
	{
		p.type = FMT_V16;
		p._imm = reg_fr_0;
	}
	else
	{
		p.type = FMT_I32;
		p._imm = reg;
	}
	return p;
}

static TYPES_INLINE shil_param shil_param_typed(u32 type, u32 imm)
{
	shil_param p;
	if (type >= FMT_REG_BASE)
		p = shil_param_reg(imm);
	else
		p = shil_param_none();
	p.type = type;
	p._imm = imm;
	return p;
}

#define shil_param_imm(v) shil_param_typed(FMT_IMM, (v))

static TYPES_INLINE bool shil_is_null(const shil_param *p) { return p->type == FMT_NULL; }
static TYPES_INLINE bool shil_is_imm(const shil_param *p) { return p->type == FMT_IMM; }
static TYPES_INLINE bool shil_is_reg(const shil_param *p) { return p->type >= FMT_REG_BASE; }
static TYPES_INLINE bool shil_is_r32i(const shil_param *p) { return p->type == FMT_I32; }
static TYPES_INLINE bool shil_is_r32f(const shil_param *p) { return p->type == FMT_F32; }
static TYPES_INLINE bool shil_is_r32(const shil_param *p) { return p->type == FMT_I32 || p->type == FMT_F32; }
static TYPES_INLINE bool shil_is_r64f(const shil_param *p) { return p->type == FMT_F64; }
static TYPES_INLINE bool shil_is_vector(const shil_param *p) { return p->type >= FMT_VECTOR_BASE; }
/* the hardware registers it is */
static TYPES_INLINE u32 shil_count(const shil_param *p)
{
	return p->type == FMT_F64 ? 2 : p->type == FMT_V2 ? 2 : p->type == FMT_V3 ? 3
			: p->type == FMT_V4 ? 4 : p->type == FMT_V8 ? 8 : p->type == FMT_V16 ? 16 : 1;
}
static TYPES_INLINE u32 shil_is_r32fv(const shil_param *p) { return p->type >= FMT_VECTOR_BASE ? shil_count(p) : 0; }
static TYPES_INLINE u32 shil_imm_value(const shil_param *p) { verify(shil_is_imm(p)); return p->_imm; }
static TYPES_INLINE Sh4RegType shil_reg(const shil_param *p) { return (Sh4RegType)p->_imm; }
static TYPES_INLINE u32 *shil_reg_ptr(const shil_param *p) { verify(shil_is_reg(p)); return GetRegPtr(p->_imm); }

struct shil_opcode
{
	shilop op;
	u32 Flow;
	u32 flags;
	u32 flags2;

	shil_param rd,rd2;
	shil_param rs1,rs2,rs3;

	u16 host_offs;
	u16 guest_offs;
	bool delay_slot;

#ifdef __cplusplus
	std::string dissasm() const;
#endif
};


#ifdef __cplusplus
extern "C" {
#endif
const char* shil_opcode_name(int op);

/* The recompiler's: a call to an operation's canonical function (C), its
 * operands and result put where the call has them */
typedef enum CanonicalParamType
{
	CPT_u32,
	CPT_u32rv,
	CPT_u64rvL,
	CPT_u64rvH,
	CPT_f32,
	CPT_f32rv,
	CPT_ptr
} CanonicalParamType;

void ngen_CC_Start(shil_opcode* op);
void ngen_CC_Param(shil_opcode* op,shil_param* par,CanonicalParamType tp);
void ngen_CC_Call(shil_opcode* op,void* function);
void ngen_CC_Finish(shil_opcode* op);

/* a register's name, an operation as text, in @buf of @size */
void shil_name_reg(char *buf, size_t size, Sh4RegType reg);
void shil_disasm(char *buf, size_t size, const shil_opcode *op);
#ifdef __cplusplus
}

static inline std::string name_reg(Sh4RegType reg)
{
	char buf[64];
	shil_name_reg(buf, sizeof(buf), reg);
	return buf;
}

inline std::string shil_opcode::dissasm() const
{
	char buf[256];
	shil_disasm(buf, sizeof(buf), this);
	return buf;
}
#endif

#endif
