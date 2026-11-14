/* The modem's UPnP (core/network/miniupnp.c, on libretro-common's
 * net_upnp). See upnp_test.sh.
 *
 *   upnp_test found     a gateway is there: found, two ports mapped (TCP
 *                       5000, UDP 5001), then let go (the gateway's log is
 *                       checked by the script)
 *   upnp_test none      no gateway: not found in its time, nothing mapped
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "types.h"
#include "network/miniupnp.h"

settings_t settings;
/* (no log) */
retro_log_printf_t log_cb;

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} } while (0)

int main(int argc, char **argv)
{
	miniupnp_t upnp = {};

	if (argc != 2)
		return 2;
	if (!strcmp(argv[1], "found"))
	{
		CHECK(miniupnp_init(&upnp));
		CHECK(miniupnp_add_port_mapping(&upnp, 5000, true));
		CHECK(miniupnp_add_port_mapping(&upnp, 5001, false));
		CHECK(!miniupnp_add_port_mapping(&upnp, 0, true));
		miniupnp_term(&upnp);
		/* (let go once: nothing left to delete) */
		miniupnp_term(&upnp);
		CHECK(!miniupnp_add_port_mapping(&upnp, 5002, true));
	}
	else
	{
		time_t start = time(NULL);
		CHECK(!miniupnp_init(&upnp));
		/* (given up in its time, a few seconds) */
		CHECK(time(NULL) - start < 10);
		CHECK(!miniupnp_add_port_mapping(&upnp, 5000, true));
		miniupnp_term(&upnp);
	}
	printf("upnp_test %s: %s\n", argv[1], failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
