/* The modem's UPnP (core/network/miniupnp.cpp, on libretro-common's
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
void GenericLog(LogTypes::LOG_LEVELS, LogTypes::LOG_TYPE, const char *, int, const char *, ...) {}

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} } while (0)

int main(int argc, char **argv)
{
	MiniUPnP upnp;

	if (argc != 2)
		return 2;
	if (!strcmp(argv[1], "found"))
	{
		CHECK(upnp.Init());
		CHECK(upnp.AddPortMapping(5000, true));
		CHECK(upnp.AddPortMapping(5001, false));
		CHECK(!upnp.AddPortMapping(0, true));
		upnp.Term();
		/* (let go once: nothing left to delete) */
		upnp.Term();
		CHECK(!upnp.AddPortMapping(5002, true));
	}
	else
	{
		time_t start = time(NULL);
		CHECK(!upnp.Init());
		/* (given up in its time, a few seconds) */
		CHECK(time(NULL) - start < 10);
		CHECK(!upnp.AddPortMapping(5000, true));
		upnp.Term();
	}
	printf("upnp_test %s: %s\n", argv[1], failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
