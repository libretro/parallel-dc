/* The NAOMI's I/O board bridge (the MIE, 315-6149): the maple device that
 * the game talks to its JVS I/O boards through, its EEPROM, the program
 * the game sends its processor and its RS-422 port. maple_devs.c's
 * maple_jamma is the device; this is all it does. */
#ifndef JVS_BRIDGE_H
#define JVS_BRIDGE_H
#include "types.h"
#include "jvs_board.h"
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

struct mcfg_map;

typedef struct jvs_bridge
{
	/* (on its bus, in it: made as it is) */
	jvs_boards boards;
	/* The program the game sent is one that reads the requests another
	 * way (crazy_mode), The House of the Dead 2 prototype's (hotd2p). A
	 * state does not have the second: on a load it is worked out from the
	 * game's name. */
	bool crazy_mode;
	bool hotd2p;
	/* the EEPROM has been written to since it was last saved */
	bool eeprom_dirty;
	/* each node's stored request, its length first; each channel's answers waiting */
	u8 repeat_request[32][256];
	u8 receive_buffer[32][258];
	u32 receive_length[32];
} jvs_bridge;

/* A bridge as one is on a cabinet: its boards there from the start (made
 * only when the game first spoke to them, a state saved before that was
 * smaller than one saved after, and a frontend asks the size once) */
void jvs_bridge_init(jvs_bridge *br);
/* A maple frame for the bridge, @buffer_in_len bytes: its answer in
 * @buffer_out, and the answer's length. The inputs are @config's. */
u32 jvs_bridge_dma(jvs_bridge *br, const struct mcfg_map *config,
		const u32 *buffer_in, u32 buffer_in_len, u32 *buffer_out);
/* Its part of a state, in a maple device's */
bool jvs_bridge_serialize(const jvs_bridge *br, void **data, unsigned int *total_size);
bool jvs_bridge_unserialize(jvs_bridge *br, void **data, unsigned int *total_size);

/* The EEPROM read from its file (eeprom_file), once: see jvs_bridge.c */
void jvs_eeprom_load(void);

/* The bridge of the machine, if it has one (maple_devs.c: the first
 * port's): the one in a save state */
jvs_bridge *maple_jvs_bridge(void);

RETRO_END_DECLS

#endif
