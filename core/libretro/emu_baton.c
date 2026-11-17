/* Who has the emulated machine (emu_baton.h) */
#include "emu_baton.h"

emu_baton_t emu_baton;

bool emu_baton_init(emu_baton_t *b)
{
	retro_atomic_int_init(&b->state, EMU_BATON_NONE);
	retro_atomic_ptr_init(&b->work, NULL);
	retro_atomic_int_init(&b->acked, 0);
	retro_atomic_int_init(&b->busy, 0);
	return retro_eventcount_init(&b->ec);
}

void emu_baton_free(emu_baton_t *b)
{
	retro_eventcount_free(&b->ec);
}

/* Sleep until *flag is non-zero, or zero when @until_zero. */
static void sleep_while_zero(emu_baton_t *b, retro_atomic_int_t *flag, bool until_zero)
{
	for (;;)
	{
		int key;

		if ((retro_atomic_load_acquire_int(flag) == 0) == until_zero)
			return;
		key = retro_eventcount_prepare_wait(&b->ec);
		if ((retro_atomic_load_acquire_int(flag) == 0) == until_zero)
			retro_eventcount_cancel_wait(&b->ec);
		else
			retro_eventcount_commit_wait(&b->ec, key);
	}
}

void emu_baton_hold(emu_baton_t *b)
{
	for (;;)
	{
		int key;

		if (retro_atomic_cas_int(&b->state, EMU_BATON_NONE, EMU_BATON_HELD))
			return;
		key = retro_eventcount_prepare_wait(&b->ec);
		if (retro_atomic_load_acquire_int(&b->state) == EMU_BATON_NONE)
			retro_eventcount_cancel_wait(&b->ec);
		else
			retro_eventcount_commit_wait(&b->ec, key);
	}
}

/* @state set, and whoever sleeps on it told */
static void set_state(emu_baton_t *b, int state)
{
	retro_atomic_store_release_int(&b->state, state);
	retro_eventcount_notify(&b->ec);
}

void emu_baton_release(emu_baton_t *b)
{
	set_state(b, EMU_BATON_NONE);
}

void emu_baton_start_frame(emu_baton_t *b)
{
	set_state(b, EMU_BATON_FRAME);
}

void *emu_baton_wait_work(emu_baton_t *b)
{
	for (;;)
	{
		int key;
		void *w = retro_atomic_load_acquire_ptr(&b->work);

		if (w != NULL)
		{
			retro_atomic_store_release_int(&b->busy, 1);
			retro_atomic_store_release_ptr(&b->work, NULL);
			return w;
		}
		if (retro_atomic_load_acquire_int(&b->state) != EMU_BATON_FRAME)
			return NULL;

		key = retro_eventcount_prepare_wait(&b->ec);
		if (retro_atomic_load_acquire_ptr(&b->work) != NULL
				|| retro_atomic_load_acquire_int(&b->state) != EMU_BATON_FRAME)
			retro_eventcount_cancel_wait(&b->ec);
		else
			retro_eventcount_commit_wait(&b->ec, key);
	}
}

void emu_baton_ack_work(emu_baton_t *b)
{
	retro_atomic_store_release_int(&b->acked, 1);
	retro_eventcount_notify(&b->ec);
}

void emu_baton_work_done(emu_baton_t *b)
{
	retro_atomic_store_release_int(&b->busy, 0);
	retro_eventcount_notify(&b->ec);
}

void emu_baton_tell_thread_to_exit(emu_baton_t *b)
{
	set_state(b, EMU_BATON_EXIT);
}

void emu_baton_thread_gone(emu_baton_t *b)
{
	set_state(b, EMU_BATON_NONE);
}

bool emu_baton_wait_for_frame(emu_baton_t *b)
{
	for (;;)
	{
		int key, st = retro_atomic_load_acquire_int(&b->state);

		if (st == EMU_BATON_FRAME)
			return true;
		if (st == EMU_BATON_EXIT)
			return false;

		key = retro_eventcount_prepare_wait(&b->ec);
		st  = retro_atomic_load_acquire_int(&b->state);
		if (st == EMU_BATON_FRAME || st == EMU_BATON_EXIT)
			retro_eventcount_cancel_wait(&b->ec);
		else
			retro_eventcount_commit_wait(&b->ec, key);
	}
}

bool emu_baton_frame_running(emu_baton_t *b)
{
	return retro_atomic_load_acquire_int(&b->state) == EMU_BATON_FRAME;
}

void emu_baton_end_frame(emu_baton_t *b)
{
	set_state(b, EMU_BATON_HELD);
}

void emu_baton_wait_work_done(emu_baton_t *b)
{
	sleep_while_zero(b, &b->busy, true);
}

void emu_baton_hand_over(emu_baton_t *b, void *w)
{
	retro_atomic_store_release_int(&b->acked, 0);
	retro_atomic_store_release_ptr(&b->work, w);
	retro_eventcount_notify(&b->ec);
	sleep_while_zero(b, &b->acked, false);
}
