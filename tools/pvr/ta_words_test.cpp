/* ta_words.h against ta_structs.h's unions: every field of every word,
 * read and changed, the same both ways, for words of every bit. See
 * sorter_run.sh. */
#include <stdio.h>
#include "hw/pvr/ta_structs.h"

static unsigned bad;
static u32 rng = 0x9E3779B9u;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

#define CHECK(U, F) \
	do { U u; u32 v = rnd(); u.full = w; \
		if (u.F != TAW_GET(w, U##_##F)) { printf("%s.%s read\n", #U, #F); bad++; } \
		u.F = v; \
		if (u.full != TAW_SET(w, U##_##F, v)) { printf("%s.%s written\n", #U, #F); bad++; } \
	} while (0)

int main(void)
{
	int i;
	for (i = 0; i < 4096; i++)
	{
		const u32 w = i < 32 ? (u32)1 << i : i < 64 ? ~((u32)1 << (i - 32)) : rnd();
		CHECK(PCW, UV_16bit);
		CHECK(PCW, Gouraud);
		CHECK(PCW, Offset);
		CHECK(PCW, Texture);
		CHECK(PCW, Col_Type);
		CHECK(PCW, Volume);
		CHECK(PCW, Shadow);
		CHECK(PCW, User_Clip);
		CHECK(PCW, Strip_Len);
		CHECK(PCW, Group_En);
		CHECK(PCW, ListType);
		CHECK(PCW, EndOfStrip);
		CHECK(PCW, ParaType);
		CHECK(PCW, S6X);
		CHECK(PCW, PTEOS);
		CHECK(ISP_TSP, DCalcCtrl);
		CHECK(ISP_TSP, CacheBypass);
		CHECK(ISP_TSP, UV_16b);
		CHECK(ISP_TSP, Gouraud);
		CHECK(ISP_TSP, Offset);
		CHECK(ISP_TSP, Texture);
		CHECK(ISP_TSP, ZWriteDis);
		CHECK(ISP_TSP, CullMode);
		CHECK(ISP_TSP, DepthMode);
		CHECK(ISP_Modvol, id);
		CHECK(ISP_Modvol, VolumeLast);
		CHECK(ISP_Modvol, CullMode);
		CHECK(ISP_Modvol, DepthMode);
		CHECK(TSP, TexV);
		CHECK(TSP, TexU);
		CHECK(TSP, ShadInstr);
		CHECK(TSP, MipMapD);
		CHECK(TSP, SupSample);
		CHECK(TSP, FilterMode);
		CHECK(TSP, ClampV);
		CHECK(TSP, ClampU);
		CHECK(TSP, FlipV);
		CHECK(TSP, FlipU);
		CHECK(TSP, IgnoreTexA);
		CHECK(TSP, UseAlpha);
		CHECK(TSP, ColorClamp);
		CHECK(TSP, FogCtrl);
		CHECK(TSP, DstSelect);
		CHECK(TSP, SrcSelect);
		CHECK(TSP, DstInstr);
		CHECK(TSP, SrcInstr);
		CHECK(TCW, TexAddr);
		CHECK(TCW, StrideSel);
		CHECK(TCW, ScanOrder);
		CHECK(TCW, PixelFmt);
		CHECK(TCW, VQ_Comp);
		CHECK(TCW, MipMapped);
		CHECK(TCW, PalSelect);
	}
	printf("ta_words: %u fields, %s\n", 53u, bad ? "NOT as the unions" : "as the unions");
	return bad != 0;
}
