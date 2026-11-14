/* The ARM7 interpreter's names, which its instructions (arm-new.h) are
 * written with: for arm7.c, and the 32-bit ARM host's recompiler
 * (arm7_rec_arm32.cpp), which runs some instructions through it. */
#pragma once
#include "arm7.h"
#include "arm_mem.h"

#define arm_printf(...) DEBUG_LOG(AICA_ARM, __VA_ARGS__)

#define CPUReadMemoryQuick(addr) (*(u32*)&aica_ram.data[(addr)&ARAM_MASK])
#define CPUReadByte arm_ReadMem8
#define CPUReadMemory arm_ReadMem32
#define CPUReadHalfWord arm_ReadMem16
#define CPUReadHalfWordSigned(addr) ((s16)arm_ReadMem16(addr))

#define CPUWriteMemory arm_WriteMem32
#define CPUWriteHalfWord arm_WriteMem16
#define CPUWriteByte arm_WriteMem8


#define reg arm_Reg
#define armNextPC reg[R15_ARM_NEXT].I


#define CPUUpdateTicksAccesint(a) 1
#define CPUUpdateTicksAccessSeq32(a) 1
#define CPUUpdateTicksAccesshort(a) 1
#define CPUUpdateTicksAccess32(a) 1
#define CPUUpdateTicksAccess16(a) 1

#define N_FLAG (reg[RN_PSR_FLAGS].FLG.N)
#define Z_FLAG (reg[RN_PSR_FLAGS].FLG.Z)
#define C_FLAG (reg[RN_PSR_FLAGS].FLG.C)
#define V_FLAG (reg[RN_PSR_FLAGS].FLG.V)

#ifdef __cplusplus
extern "C" {
#endif

void CPUSwitchMode(int mode, bool saveState);
void CPUUpdateFlags(void);
void CPUSoftwareInterrupt(int comment);
void CPUUndefinedException(void);

#ifdef __cplusplus
}
#endif
