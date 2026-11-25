/* The tile accelerator's control words as bit-field unions, the way the
 * old C++ core had them: what tools/pvr/ta_words_test.c checks
 * core/hw/pvr/ta_words.h's macros against, and the reference renderer
 * (tools/rend/refsw) reads the words with. */
#pragma once
#include "types.h"
#include "hw/pvr/ta_words.h"

typedef union PCW PCW;
typedef union ISP_TSP ISP_TSP;
typedef union ISP_Modvol ISP_Modvol;
typedef union TSP TSP;
typedef union TCW TCW;

#pragma pack(push, 1)
union PCW
{
	struct
	{
#ifdef MSB_FIRST
		/* Para Control */
		u32 ParaType    : 3;
		u32 EndOfStrip  : 1;
		u32 Res_1       : 1;
		u32 ListType    : 3;

		/* Group Control */
		u32 Group_En    : 1;
		u32 Res_2       : 3;
		u32 Strip_Len   : 2;
		u32 User_Clip   : 2;

		u32 Reserved    : 8; /* 0000 0000 */

		/* Obj Control (affects drawing?) */
		u32 Shadow      : 1; /* 1 */
		u32 Volume      : 1; /* 1 */
		u32 Col_Type    : 2; /* 00 */
		u32 Texture     : 1; /* 1 */
		u32 Offset      : 1; /* 1 */
		u32 Gouraud     : 1; /* 1 */
		u32 UV_16bit    : 1; /* 0 */
#else
		/* Obj Control (affects drawing?) */
		u32 UV_16bit    : 1; /* 0 */
		u32 Gouraud     : 1; /* 1 */
		u32 Offset      : 1; /* 1 */
		u32 Texture     : 1; /* 1 */
		u32 Col_Type    : 2; /* 00 */
		u32 Volume      : 1; /* 1 */
		u32 Shadow      : 1; /* 1 */

		u32 Reserved    : 8; /* 0000 0000 */

		/* Group Control */
		u32 User_Clip   : 2;
		u32 Strip_Len   : 2;
		u32 Res_2       : 3;
		u32 Group_En    : 1;

		/* Para Control */
		u32 ListType    : 3;
		u32 Res_1       : 1;
		u32 EndOfStrip  : 1;
		u32 ParaType    : 3;
#endif
	};
	u8 obj_ctrl;
	struct
	{
		u32 padin  : 8;
		u32 S6X    : 1;    /* set by TA preprocessing if sz64 */
		u32 padin2 : 19;
		u32 PTEOS  : 4;
	};
	u32 full;
};

/* ISP/TSP Instruction Word */

union ISP_TSP
{
	struct
	{
#ifdef MSB_FIRST
		u32 DepthMode   : 3;
		u32 CullMode    : 2;
		u32 ZWriteDis   : 1;
		u32 Texture     : 1; /* -- up to here -- */
		u32 Offset      : 1;
		u32 Gouraud     : 1; /* by the ones on PCW */
		u32 UV_16b      : 1; /* In TA they are replaced */
		u32 CacheBypass : 1;
		u32 DCalcCtrl   : 1;
		u32 Reserved    : 20;
#else
		u32 Reserved    : 20;
		u32 DCalcCtrl   : 1;
		u32 CacheBypass : 1;
		u32 UV_16b      : 1; /* In TA they are replaced */
		u32 Gouraud     : 1; /* by the ones on PCW */
		u32 Offset      : 1;
		u32 Texture     : 1; /* -- up to here -- */
		u32 ZWriteDis   : 1;
		u32 CullMode    : 2;
		u32 DepthMode   : 3;
#endif
	};
	u32 full;
};

union ISP_Modvol
{
	struct
	{
#ifdef MSB_FIRST
		u32 DepthMode  : 3;
		u32 CullMode   : 2;
		u32 VolumeLast : 1;
		u32 id         : 26;
#else
		u32 id         : 26;
		u32 VolumeLast : 1;
		u32 CullMode   : 2;
		u32 DepthMode  : 3;
#endif
	};
	u32 full;
};


/* END ISP/TSP Instruction Word */


/* TSP Instruction Word */

union TSP
{
	struct
	{
#ifdef MSB_FIRST
		u32 SrcInstr    : 3;
		u32 DstInstr    : 3;
		u32 SrcSelect   : 1; /* Primary Accum */
		u32 DstSelect   : 1; /* Secondary Accum */
		u32 FogCtrl     : 2;
		u32 ColorClamp  : 1;
		u32 UseAlpha    : 1;
		u32 IgnoreTexA  : 1;
		u32 FlipU       : 1;
		u32 FlipV       : 1;
		u32 ClampU      : 1;
		u32 ClampV      : 1;
		u32 FilterMode  : 2;
		u32 SupSample   : 1;
		u32 MipMapD     : 4;
		u32 ShadInstr   : 2;
		u32 TexU        : 3;
		u32 TexV        : 3;
#else
		u32 TexV        : 3;
		u32 TexU        : 3;
		u32 ShadInstr   : 2;
		u32 MipMapD     : 4;
		u32 SupSample   : 1;
		u32 FilterMode  : 2;
		u32 ClampV      : 1;
		u32 ClampU      : 1;
		u32 FlipV       : 1;
		u32 FlipU       : 1;
		u32 IgnoreTexA  : 1;
		u32 UseAlpha    : 1;
		u32 ColorClamp  : 1;
		u32 FogCtrl     : 2;
		u32 DstSelect   : 1; /* Secondary Accum */
		u32 SrcSelect   : 1; /* Primary Accum */
		u32 DstInstr    : 3;
		u32 SrcInstr    : 3;
#endif
	};
	u32 full;
} ;


/* END TSP Instruction Word */


/* Texture Control Word */
union TCW
{
	struct
	{
#ifdef MSB_FIRST
		u32 MipMapped : 1;
		u32 VQ_Comp   : 1;
		u32 PixelFmt  : 3;
		u32 ScanOrder : 1;
		u32 StrideSel : 1;
		u32 Reserved  : 4;
		u32 TexAddr   :21;
#else
		u32 TexAddr   :21;
		u32 Reserved  : 4;
		u32 StrideSel : 1;
		u32 ScanOrder : 1;
		u32 PixelFmt  : 3;
		u32 VQ_Comp   : 1;
		u32 MipMapped : 1;
#endif
	} ;
	struct
	{
#ifdef MSB_FIRST
		u32 PalSelect : 6;
		u32 pading_0  :21;
#else
		u32 pading_0  :21;
		u32 PalSelect : 6;
#endif
	} ;
	u32 full;
};

#pragma pack(pop)
