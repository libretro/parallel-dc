/* What the core reads of a disc's IP.BIN (core/reios/reios.h): its
 * product number, as the table of games' own settings looks it up, and
 * its areas, its Windows CE, VGA and MIL-CD marks. */
#include <stdio.h>
#include <string.h>
#include "reios/reios.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { \
   printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* An IP.BIN of spaces, as a disc pads its fields */
static void blank(ip_meta_t *m)
{
   memset(m, ' ', sizeof(*m));
}

static void set(char *field, size_t size, const char *value)
{
   memset(field, ' ', size);
   memcpy(field, value, strlen(value));
}

static void check_id(const char *field, const char *trimmed, const char *raw)
{
   ip_meta_t m;
   char id[sizeof(m.product_number) + 1];
   char prod_id[sizeof(m.product_number) + 1];

   blank(&m);
   memcpy(m.product_number, field, sizeof(m.product_number));
   memset(id, 'x', sizeof(id));
   memset(prod_id, 'x', sizeof(prod_id));
   ip_meta_product_id(&m, id, prod_id);
   if (strcmp(id, trimmed) || strcmp(prod_id, raw))
   {
      printf("FAIL product number \"%.10s\": \"%s\" \"%s\", not \"%s\" \"%s\"\n",
            field, id, prod_id, trimmed, raw);
      failures++;
   }
   /* (without the field as it is) */
   memset(id, 'x', sizeof(id));
   ip_meta_product_id(&m, id, NULL);
   CHECK(!strcmp(id, trimmed));
}

int main(void)
{
   ip_meta_t m;

   check_id("T-8102N   ", "T-8102N", "T-8102N   ");
   /* the spaces inside it are its own */
   check_id("T7001D  50", "T7001D  50", "T7001D  50");
   check_id("T40505D 50", "T40505D 50", "T40505D 50");
   /* all ten characters, with no end of its own */
   check_id("MK-5100050", "MK-5100050", "MK-5100050");
   check_id("          ", "", "          ");
   /* a NUL ends it */
   check_id("AB\0CD     ", "AB", "AB");
   check_id("\0         ", "", "");

   blank(&m);
   set(m.area_symbols, sizeof(m.area_symbols), "JUE");
   CHECK(ip_meta_has_area(&m, 'J'));
   CHECK(ip_meta_has_area(&m, 'U'));
   CHECK(ip_meta_has_area(&m, 'E'));
   set(m.area_symbols, sizeof(m.area_symbols), " U");
   CHECK(!ip_meta_has_area(&m, 'J'));
   CHECK(ip_meta_has_area(&m, 'U'));
   CHECK(!ip_meta_has_area(&m, 'E'));
   /* the field's last character, and one after a NUL: the whole field is read */
   memset(m.area_symbols, ' ', sizeof(m.area_symbols));
   m.area_symbols[0] = '\0';
   m.area_symbols[sizeof(m.area_symbols) - 1] = 'E';
   CHECK(ip_meta_has_area(&m, 'E'));
   CHECK(!ip_meta_has_area(&m, 'J'));
   memset(m.area_symbols, ' ', sizeof(m.area_symbols));
   CHECK(!ip_meta_has_area(&m, 'J') && !ip_meta_has_area(&m, 'U') && !ip_meta_has_area(&m, 'E'));
   /* (the next field is not the areas') */
   m.ctrl[0] = 'U';
   CHECK(!ip_meta_has_area(&m, 'U'));

   blank(&m);
   CHECK(!ip_meta_is_wince(&m));
   m.wince = '1';
   CHECK(ip_meta_is_wince(&m));
   blank(&m);
   set(m.boot_filename, sizeof(m.boot_filename), "0WINCEOS.BIN");
   CHECK(ip_meta_is_wince(&m));
   set(m.boot_filename, sizeof(m.boot_filename), "1ST_READ.BIN");
   CHECK(!ip_meta_is_wince(&m));

   blank(&m);
   CHECK(!ip_meta_supports_vga(&m));
   m.vga = '1';
   CHECK(ip_meta_supports_vga(&m));

   blank(&m);
   set(m.disk_type, sizeof(m.disk_type), "GD-ROM");
   CHECK(!ip_meta_is_milcd(&m));
   set(m.disk_type, sizeof(m.disk_type), "MIL CD");
   CHECK(ip_meta_is_milcd(&m));

   if (failures)
   {
      printf("ip_meta: %d failed\n", failures);
      return 1;
   }
   printf("ip_meta: all passed\n");
   return 0;
}
