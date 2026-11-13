/* The modem's DNS queries and answers (core/hw/modem/dns.c), by themselves:
 * the query for a name as it goes on the wire, and the answers it takes the
 * address from, with every kind of answer cut short or lying about its
 * lengths read no further than it goes (sh tools/modem/dns_run.sh). */
#include <stdio.h>
#include <string.h>
#include "hw/modem/dns.h"

static int fails;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const u8 query_auriga[] = {
	0, 0, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
	6, 'a', 'u', 'r', 'i', 'g', 'a', 8, 's', 'e', 'g', 'a', 's', 'o', 'f', 't', 3, 'c', 'o', 'm', 0,
	0, 1, 0, 1
};

/* the query, then answers */
static int answer(u8 *msg, const u8 *answers, int alen, int ancount)
{
	int name_len;
	int len = dns_query_build(msg, 512, 0x1234, "auriga.segasoft.com", &name_len);
	msg[2] = 0x81; msg[3] = 0x80;
	msg[6] = (u8)(ancount >> 8); msg[7] = (u8)ancount;
	memcpy(msg + len, answers, alen);
	return len + alen;
}

int main(void)
{
	u8 buf[512], msg[1024];
	int name_len = 0, len;
	u32 addr;
	u16 id = 0;

	/* the query as on the wire (the id in the host's order) */
	len = dns_query_build(buf, sizeof(buf), id, "auriga.segasoft.com", &name_len);
	CHECK(len == (int)sizeof(query_auriga));
	CHECK(name_len == 21);
	CHECK(memcmp(buf, query_auriga, sizeof(query_auriga)) == 0);
	/* names that make no query */
	CHECK(dns_query_build(buf, sizeof(buf), id, "", &name_len) == 0);
	CHECK(dns_query_build(buf, sizeof(buf), id, "a..b", &name_len) == 0);
	CHECK(dns_query_build(buf, sizeof(buf), id, "a.", &name_len) == 0);
	CHECK(dns_query_build(buf, sizeof(buf), id, ".a", &name_len) == 0);
	{
		char longlabel[70];
		memset(longlabel, 'x', 64); longlabel[64] = 0;
		CHECK(dns_query_build(buf, sizeof(buf), id, longlabel, &name_len) == 0);
		longlabel[63] = 0;
		CHECK(dns_query_build(buf, sizeof(buf), id, longlabel, &name_len) == 12 + 65 + 4);
	}
	{
		char longname[200];
		int i;
		for (i = 0; i < 199; i++)
			longname[i] = (i % 10 == 9) ? '.' : 'y';
		longname[199] = 0;
		CHECK(dns_query_build(buf, sizeof(buf), id, longname, &name_len) == 0);
		longname[126] = 0;
		CHECK(dns_query_build(buf, sizeof(buf), id, longname, &name_len) == 12 + 128 + 4);
		CHECK(dns_query_build(buf, 12 + 128 + 3, id, longname, &name_len) == 0);
	}
	CHECK(dns_query_build(buf, sizeof(query_auriga) - 1, id, "auriga.segasoft.com", &name_len) == 0);

	{
		/* a CNAME by pointer, then the address */
		static const u8 a1[] = {
			0xC0, 0x0C, 0, 5, 0, 1, 0, 0, 0, 60, 0, 2, 0xC0, 0x0C,
			0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 203, 0, 113, 7
		};
		/* the address behind a name in labels */
		static const u8 a2[] = {
			3, 'f', 'o', 'o', 0, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 10, 1, 2, 3
		};
		const u8 want1[4] = { 203, 0, 113, 7 }, want2[4] = { 10, 1, 2, 3 };
		int n, cut;

		n = answer(msg, a1, sizeof(a1), 2);
		addr = 0;
		CHECK(dns_answer_parse(msg, n, 21, &addr) == 0);
		CHECK(memcmp(&addr, want1, 4) == 0);
		/* every message cut short of the address has none */
		for (cut = 0; cut < n; cut++)
			CHECK(dns_answer_parse(msg, cut, 21, &addr) == -1);
		/* one answer said where there are two: the CNAME only */
		n = answer(msg, a1, sizeof(a1), 1);
		CHECK(dns_answer_parse(msg, n, 21, &addr) == -1);
		/* more said than there are */
		n = answer(msg, a1, sizeof(a1), 0xFFFF);
		CHECK(dns_answer_parse(msg, n, 21, &addr) == 0);
		n = answer(msg, a2, sizeof(a2), 1);
		CHECK(dns_answer_parse(msg, n, 21, &addr) == 0);
		CHECK(memcmp(&addr, want2, 4) == 0);
		for (cut = 0; cut < n; cut++)
			CHECK(dns_answer_parse(msg, cut, 21, &addr) == -1);
	}
	{
		/* lengths that lie: a record longer than the message, a label
		 * past its end, a label length of a kind not known, a pointer cut
		 * in half, an address record too short to have one */
		static const u8 bad[][16] = {
			{ 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0xFF, 0xFF, 1, 2, 3, 4 },
			{ 60, 'a', 'b', 0, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 1, 2 },
			{ 0x40, 'a', 0, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 1, 2, 3 },
			{ 0xC0 },
			{ 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 3, 1, 2, 3, 0 }
		};
		static const int bad_len[] = { 16, 16, 16, 1, 15 };
		int i;
		for (i = 0; i < 5; i++)
		{
			int n = answer(msg, bad[i], bad_len[i], 1);
			CHECK(dns_answer_parse(msg, n, 21, &addr) == -1);
		}
	}
	if (fails)
	{
		printf("dns_test: %d failed\n", fails);
		return 1;
	}
	printf("dns_test: queries and answers as they should be\n");
	return 0;
}
