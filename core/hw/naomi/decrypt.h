#ifndef __DECRYPT_STV_H__
#define __DECRYPT_STV_H__

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

u16 cryptoDecrypt(void);
void cyptoSetKey(u32 privKey);
void cyptoSetLowAddr(u16 val);
void cyptoSetHighAddr(u16 val);
void cyptoSetSubkey(u16 subKey);
void cryptoReset(void);
void cryptoSerialize(void **data, unsigned int *total_size);
void cryptoUnserialize(void **data, unsigned int *total_size);

/* The M2 cartridge's ciphered data, the 16-bit word at @addr (naomi_cart.cpp) */
u16 naomi_m2_read_ciphered(u32 addr);

#ifdef __cplusplus
}
#endif

#endif
