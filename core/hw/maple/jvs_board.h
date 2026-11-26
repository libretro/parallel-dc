/* The NAOMI's JVS I/O boards, as the I/O board bridge (jvs_bridge.c)
 * has them on its bus: each kind of board the cabinets
 * have, what it answers a request with, and its part of a save state. */
#ifndef JVS_BOARD_H
#define JVS_BOARD_H
#include "types.h"
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

struct mcfg_map;

/* The kinds of board, by part number and by the game wired to one */
enum
{
	JVS_BOARD_837_13551,		/* the most common */
	JVS_BOARD_837_13551_NOANALOG,
	JVS_BOARD_837_13551_4P,		/* the same in four-player mode */
	JVS_BOARD_837_13551_MUSHIKING,
	JVS_BOARD_837_13938,		/* rotary encoders: Virtua Golf, Outtrigger */
	JVS_BOARD_837_13938_SHOOTOUT,
	JVS_BOARD_837_13938_KICK4CASH,
	JVS_BOARD_837_13938_CRACKINDJ,
	JVS_BOARD_837_13844,		/* Sega Marine Fishing */
	JVS_BOARD_837_13844_ENCODERS,
	JVS_BOARD_837_13844_TOUCH,
	JVS_BOARD_837_13844_WRUNGP,	/* Wave Runner GP's drive board, made up */
	JVS_BOARD_837_13844_RACING,	/* F355 Challenge's and Tokyo Bus Guide's drive board */
	JVS_BOARD_837_13844_18WHEELER,
	JVS_BOARD_837_13844_SAMBA,
	JVS_BOARD_NAMCO_JYU,		/* Ninja Assault */
	JVS_BOARD_NAMCO_FCB,		/* Mazan */
	JVS_BOARD_NAMCO_FCA,		/* Gun Survivor */
	JVS_BOARD_NAMCO_V226,		/* World Kicks */
	JVS_BOARD_NAMCO_V226_PCB
};

typedef struct jvs_board
{
	u8 kind;
	u8 node_id;
	/* whose inputs the board has: the players' from this one on */
	u8 first_player;
	bool lightgun_as_analog;
	u32 player_count;
	u32 digital_in_count;
	u32 coin_input_count;
	u32 analog_count;
	u32 encoder_count;
	u32 light_gun_count;
	u32 output_count;
	bool init_in_progress;
	/* the first byte of the outputs the game last set (lamps, motors) */
	u8 output0;
	/* a chute for each player a board can have */
	u32 coin_count[4];
	bool coin_chute[4];

	/* The kinds' own: Mushiking's test switch seen down at the last read */
	u8 test_down;
	/* ...what a drive board last answered */
	u8 drive_board;
	/* ...a racing board's drive board: Tokyo Bus Guide's, in test mode,
	 * told to work, the damper's high bits, the motor's power (percent),
	 * the test menu's roll (-7 to 7) */
	bool tokyo_bus;
	bool test_mode;
	bool active;
	u8 damper_high;
	u8 power;
	s8 torque;
	/* ...18 Wheeler's shifter: the gear (0 low, 1 high, 0xff reverse) */
	u8 gear;
	bool shift_held;
	/* ...World Kicks' stick last read */
	s8 axis_y;
} jvs_board;

/* The most boards a cabinet has */
#define JVS_BOARDS_MAX 2

typedef struct jvs_boards
{
	jvs_board board[JVS_BOARDS_MAX];
	u32 count;
	/* the two boards' players follow the cabinet setting in the EEPROM
	 * (jvs_boards_order()) */
	bool cabinet_order;
} jvs_boards;

/* The boards of the cabinet the game is for (settings.mapping.JammaSetup,
 * naomi_game_id), if there are none yet */
void jvs_boards_create(jvs_boards *b);
/* The players on each of the two boards, from the cabinet setting in the
 * EEPROM: set for three or four players, the game has players 1 and 2 on
 * the second board (node 2) and 3 and 4 on the first */
void jvs_boards_order(jvs_boards *b);
/* Board @index's answer to @in (@len bytes, and four zeroes after them) in
 * @out (256 bytes); its length, 0 for none. The inputs are @config's. */
u32 jvs_board_message(jvs_boards *b, u32 index, const struct mcfg_map *config,
		const u8 *in, u32 len, u8 *out);
/* A board's part of the bridge's state (its address, the light gun read as
 * sticks) */
bool jvs_board_serialize(const jvs_board *board, void **data, unsigned int *total_size);
bool jvs_board_unserialize(jvs_board *board, void **data, unsigned int *total_size);

/* The game has its two boards' players the other way round on a cabinet
 * set for three or four players */
bool jvs_dual_reversed_game(void);

/* The boards of the NAOMI's I/O board bridge, if the machine has one
 * (jvs_bridge.c): the boards in a save state */
jvs_boards *maple_jvs_boards(void);

RETRO_END_DECLS

#endif
