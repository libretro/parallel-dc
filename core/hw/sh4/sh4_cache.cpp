/* The interpreter's instruction and operand caches (sh4_cache.h), and what
 * C has of them */
#include "sh4_cache.h"

sh4_icache icache;
sh4_ocache ocache;

void sh4_icache_invalidate(void)
{
	icache.Invalidate();
}

void sh4_ocache_invalidate(void)
{
	ocache.Invalidate();
}

u32 sh4_icache_read_address_array(u32 addr) { return icache.ReadAddressArray(addr); }
void sh4_icache_write_address_array(u32 addr, u32 data) { icache.WriteAddressArray(addr, data); }
u32 sh4_icache_read_data_array(u32 addr) { return icache.ReadDataArray(addr); }
void sh4_icache_write_data_array(u32 addr, u32 data) { icache.WriteDataArray(addr, data); }
u32 sh4_ocache_read_address_array(u32 addr) { return ocache.ReadAddressArray(addr); }
void sh4_ocache_write_address_array(u32 addr, u32 data) { ocache.WriteAddressArray(addr, data); }
u32 sh4_ocache_read_data_array(u32 addr) { return ocache.ReadDataArray(addr); }
void sh4_ocache_write_data_array(u32 addr, u32 data) { ocache.WriteDataArray(addr, data); }
void sh4_ocache_write_back(u32 addr, bool write_back, bool invalidate) { ocache.WriteBack(addr, write_back, invalidate); }
void sh4_ocache_prefetch(u32 addr) { ocache.Prefetch(addr); }

void sh4_caches_reset(bool hard)
{
	icache.Reset(hard);
	ocache.Reset(hard);
}
