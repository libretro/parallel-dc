/* Test for the NAOMI network's waits. See run.sh.
 *
 * Builds the real core/network/naomi_network.c and runs a server and a
 * client of it, in two processes, over the loopback interface: the server
 * waits for players, the client finds it and connects, and a packet goes
 * each way round the ring. Then each side waits for a packet that never
 * comes and is told to stop.
 *
 * What is checked besides the packets: none of those waits burns CPU (each
 * side spends nearly all its time waiting and uses next to none), and a
 * wait ends as soon as naomi_network_shutdown() is called rather than at
 * its next look. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <features/features_cpu.h>
#include <rthreads/rthreads.h>
#include <retro_timers.h>

#include "types.h"
#include "network/naomi_network.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} } while (0)

settings_t settings;
/* (no log) */
retro_log_printf_t log_cb;

#define PACKET 600

static naomi_network net;
static bool is_server;
static retro_atomic_int_t settled, done;
static retro_time_t woke;

/* What the board's network thread does with the network. */
static void network_thread(void *unused)
{
	u8 buf[PACKET];
	const u8 mine = is_server ? 0xa5 : 0x5a, theirs = is_server ? 0x5a : 0xa5;
	int i;

	/* The server waits ten seconds for players; the client connects and
	 * waits for the server to start. */
	if (!naomi_network_start(&net))
	{
		CHECK(!"startNetwork");
		retro_atomic_store_release_int(&settled, 1);
		return;
	}
	CHECK(net.slot_count == 2 && net.slot_id == (is_server ? 0 : 1));

	/* The client has the token first. */
	if (!is_server)
	{
		CHECK(net.got_token);
		memset(buf, mine, PACKET);
		naomi_network_send(&net, buf, PACKET);
	}
	naomi_network_wait_for_data(&net);
	CHECK(naomi_network_receive(&net, buf, PACKET));
	for (i = 0; i < PACKET; i++)
		if (buf[i] != theirs)
		{
			CHECK(!"packet contents");
			break;
		}
	if (is_server)
	{
		CHECK(net.got_token);
		memset(buf, mine, PACKET);
		naomi_network_send(&net, buf, PACKET);
		CHECK(!net.got_token);
	}

	/* Nothing more is coming: this ends when shutdown() says so. */
	retro_atomic_store_release_int(&settled, 1);
	naomi_network_wait_for_data(&net);
	woke = cpu_features_get_time_usec();
	retro_atomic_store_release_int(&done, 1);
}

static int run_side(void)
{
	sthread_t *t;
	retro_time_t t0, stop_time;
	clock_t cpu0 = clock();
	double cpu;

	naomi_network_create(&net);
	settings.network.ActAsServer = is_server;
	if (!is_server)
	{
		strcpy(settings.network.server, "127.0.0.1");
		retro_sleep(500);	/* let the server start listening */
	}
	CHECK(naomi_network_arm_wake(&net));
	t = sthread_create(network_thread, NULL);

	t0 = cpu_features_get_time_usec();
	while (!retro_atomic_load_acquire_int(&settled)
			&& cpu_features_get_time_usec() - t0 < 40 * 1000 * 1000)
		retro_sleep(50);
	CHECK(retro_atomic_load_acquire_int(&settled));

	/* Let it get into its last wait, check it is still there, stop it. */
	retro_sleep(500);
	CHECK(!retro_atomic_load_acquire_int(&done));
	stop_time = cpu_features_get_time_usec();
	naomi_network_shutdown(&net);
	sthread_join(t);
	CHECK(retro_atomic_load_acquire_int(&done));
	/* The shutdown ended the wait itself; no timeout did. */
	CHECK(woke - stop_time < 100 * 1000);

	/* The sockets stay open a while longer. The other side is doing this
	 * same check, about now, and closing a socket under it would end its
	 * wait as surely as the shutdown does: its socket would have something
	 * to read, the end of the connection. */
	retro_sleep(1500);
	naomi_network_close_sockets(&net);
	naomi_network_terminate(&net);

	/* Seconds of waiting and no polling: hardly any CPU. */
	cpu = (double)(clock() - cpu0) / CLOCKS_PER_SEC;
	CHECK(cpu < 0.5);
	printf("%s naomi_network %s (%.2f s of CPU)\n", failures ? "FAIL" : "ok  ",
			is_server ? "server" : "client", cpu);
	return failures ? 1 : 0;
}

int main(void)
{
	int status = 0, rc;
	pid_t child = fork();

	if (child == 0)
	{
		is_server = false;
		return run_side();
	}
	is_server = true;
	rc = run_side();
	waitpid(child, &status, 0);
	return rc || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
}
