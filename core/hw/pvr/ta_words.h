#ifndef TA_WORDS_H
#define TA_WORDS_H
/* The tile accelerator's control words (ta_structs.h's PCW, ISP_TSP,
 * ISP_Modvol, TSP and TCW) as plain words, for C: each field's place
 * (_S, its lowest bit) and width (_B) in the word as the hardware has it,
 * whatever the host's byte order, and the word's field read or changed.
 * C89. C++ has the unions as well; TAW_FULL() is the word either way. */
#include "types.h"

#define TAW_MASK(f) ((((u32)1 << f##_B) - 1) << f##_S)
#define TAW_GET(w, f) (((u32)(w) >> f##_S) & (((u32)1 << f##_B) - 1))
#define TAW_SET(w, f, v) (((u32)(w) & ~TAW_MASK(f)) | (((u32)(v) << f##_S) & TAW_MASK(f)))

#ifdef __cplusplus
#define TAW_FULL(x) ((x).full)
#else
#define TAW_FULL(x) (x)
#endif

/* bits that affect drawing (for caching params) */
#ifndef PCW_DRAW_MASK
#define PCW_DRAW_MASK (0x000000CE)
#endif


#define PCW_UV_16bit_S 0
#define PCW_UV_16bit_B 1
#define PCW_Gouraud_S 1
#define PCW_Gouraud_B 1
#define PCW_Offset_S 2
#define PCW_Offset_B 1
#define PCW_Texture_S 3
#define PCW_Texture_B 1
#define PCW_Col_Type_S 4
#define PCW_Col_Type_B 2
#define PCW_Volume_S 6
#define PCW_Volume_B 1
#define PCW_Shadow_S 7
#define PCW_Shadow_B 1
#define PCW_User_Clip_S 16
#define PCW_User_Clip_B 2
#define PCW_Strip_Len_S 18
#define PCW_Strip_Len_B 2
#define PCW_Group_En_S 23
#define PCW_Group_En_B 1
#define PCW_ListType_S 24
#define PCW_ListType_B 3
#define PCW_EndOfStrip_S 28
#define PCW_EndOfStrip_B 1
#define PCW_ParaType_S 29
#define PCW_ParaType_B 3
#define PCW_S6X_S 8
#define PCW_S6X_B 1
#define PCW_PTEOS_S 28
#define PCW_PTEOS_B 4

#define ISP_TSP_DCalcCtrl_S 20
#define ISP_TSP_DCalcCtrl_B 1
#define ISP_TSP_CacheBypass_S 21
#define ISP_TSP_CacheBypass_B 1
#define ISP_TSP_UV_16b_S 22
#define ISP_TSP_UV_16b_B 1
#define ISP_TSP_Gouraud_S 23
#define ISP_TSP_Gouraud_B 1
#define ISP_TSP_Offset_S 24
#define ISP_TSP_Offset_B 1
#define ISP_TSP_Texture_S 25
#define ISP_TSP_Texture_B 1
#define ISP_TSP_ZWriteDis_S 26
#define ISP_TSP_ZWriteDis_B 1
#define ISP_TSP_CullMode_S 27
#define ISP_TSP_CullMode_B 2
#define ISP_TSP_DepthMode_S 29
#define ISP_TSP_DepthMode_B 3

#define ISP_Modvol_id_S 0
#define ISP_Modvol_id_B 26
#define ISP_Modvol_VolumeLast_S 26
#define ISP_Modvol_VolumeLast_B 1
#define ISP_Modvol_CullMode_S 27
#define ISP_Modvol_CullMode_B 2
#define ISP_Modvol_DepthMode_S 29
#define ISP_Modvol_DepthMode_B 3

#define TSP_TexV_S 0
#define TSP_TexV_B 3
#define TSP_TexU_S 3
#define TSP_TexU_B 3
#define TSP_ShadInstr_S 6
#define TSP_ShadInstr_B 2
#define TSP_MipMapD_S 8
#define TSP_MipMapD_B 4
#define TSP_SupSample_S 12
#define TSP_SupSample_B 1
#define TSP_FilterMode_S 13
#define TSP_FilterMode_B 2
#define TSP_ClampV_S 15
#define TSP_ClampV_B 1
#define TSP_ClampU_S 16
#define TSP_ClampU_B 1
#define TSP_FlipV_S 17
#define TSP_FlipV_B 1
#define TSP_FlipU_S 18
#define TSP_FlipU_B 1
#define TSP_IgnoreTexA_S 19
#define TSP_IgnoreTexA_B 1
#define TSP_UseAlpha_S 20
#define TSP_UseAlpha_B 1
#define TSP_ColorClamp_S 21
#define TSP_ColorClamp_B 1
#define TSP_FogCtrl_S 22
#define TSP_FogCtrl_B 2
#define TSP_DstSelect_S 24
#define TSP_DstSelect_B 1
#define TSP_SrcSelect_S 25
#define TSP_SrcSelect_B 1
#define TSP_DstInstr_S 26
#define TSP_DstInstr_B 3
#define TSP_SrcInstr_S 29
#define TSP_SrcInstr_B 3

#define TCW_TexAddr_S 0
#define TCW_TexAddr_B 21
#define TCW_StrideSel_S 25
#define TCW_StrideSel_B 1
#define TCW_ScanOrder_S 26
#define TCW_ScanOrder_B 1
#define TCW_PixelFmt_S 27
#define TCW_PixelFmt_B 3
#define TCW_VQ_Comp_S 30
#define TCW_VQ_Comp_B 1
#define TCW_MipMapped_S 31
#define TCW_MipMapped_B 1
#define TCW_PalSelect_S 21
#define TCW_PalSelect_B 6

/* What the words hold, for C (as the ListType_ and ParamType_ below;
 * ta.h has the pixel formats) */
#define TA_LIST_OPAQUE        0
#define TA_LIST_TRANSLUCENT   2
#define TA_LIST_PUNCH_THROUGH 4
#define TA_PARAM_SPRITE       5

/* The lists, and the parameter types */
#define ListType_Opaque 0
#define ListType_Opaque_Modifier_Volume 1
#define ListType_Translucent 2
#define ListType_Translucent_Modifier_Volume 3
#define ListType_Punch_Through 4
#define IsModVolList(list) (((list)&1)!=0)

/* (control parameters) */
#define ParamType_End_Of_List 0
#define ParamType_User_Tile_Clip 1
#define ParamType_Object_List_Set 2
/* (global parameters) */
#define ParamType_Polygon_or_Modifier_Volume 4
#define ParamType_Sprite 5
/* (vertex, sprite or modifier volume parameters) */
#define ParamType_Vertex_Parameter 7
#define ParamType_Reserved_1 3
#define ParamType_Reserved_2 6

#endif
