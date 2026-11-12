#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
	tag, as passed on sh4_sched_register
	sch_cycles, the cycle duration that the callback requested (sh4_sched_request)
	jitter, the number of cycles that the callback was delayed, [0... 448]
*/
typedef int sh4_sched_callback(int tag, int sch_cycl, int jitter);

/*
	Registed a callback to the scheduler. The returned id 
	is used for sh4_sched_request and sh4_sched_elapsed calls

	A callback that is registered already, with the same tag, is given
	the id it has, with nothing asked of it any more.
*/
int sh4_sched_register(int tag, sh4_sched_callback* ssc);

/*
	current time in SH4 cycles, referenced to boot.
	Wraps every ~21 secs
*/
u32 sh4_sched_now(void);

/*
	current time, in SH4 cycles, referenced to boot.
	Does not wrap, 64 bits.
*/
u64 sh4_sched_now64(void);

/*
	Schedule a callback to be called sh4 *cycles* after the
	invocation of this function. *Cycles* range is (0, 200M].
	
	Passing a value of 0 disables the callback.
	If called multiple times, only the last call is in effect
*/
void sh4_sched_request(int id, int cycles);

/* Whether a request made of @id is still waiting */
bool sh4_sched_is_scheduled(int id);
/* The cycles left before the callback asked for comes */
u32 sh4_sched_remaining(int id);

/*
	Tick for *cycles*
*/
void sh4_sched_tick(int cycles);

void sh4_sched_ffts(void);

typedef struct sched_list sched_list;
struct sched_list
{
	sh4_sched_callback* cb;
	int tag;
	int start;
	int end;
};

/* The events registered, by id: room for every device's, made with the
 * process (registering the same callback and tag again keeps its place) */
#define SH4_SCHED_SLOTS 128
extern sched_list sch_list[SH4_SCHED_SLOTS];
extern u32 sch_count;
/* The cycle the scheduler counts from (see sh4_sched.c) */
extern u64 sh4_sched_ffb;

#ifdef __cplusplus
}
#endif
