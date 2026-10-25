#ifndef LIBRETRO_CORE_OPTIONS_H__
#define LIBRETRO_CORE_OPTIONS_H__

#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <retro_inline.h>

#ifndef HAVE_NO_LANGEXTRA
#include "libretro_core_options_intl.h"
#endif

/*
 ********************************
 * VERSION: 2.0
 ********************************
 *
 * - 2.0: Add support for core options v2 interface
 * - 1.3: Move translations to libretro_core_options_intl.h
 *        - libretro_core_options_intl.h includes BOM and utf-8
 *          fix for MSVC 2010-2013
 *        - Added HAVE_NO_LANGEXTRA flag to disable translations
 *          on platforms/compilers without BOM support
 * - 1.2: Use core options v1 interface when
 *        RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION is >= 1
 *        (previously required RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION == 1)
 * - 1.1: Support generation of core options v0 retro_core_option_value
 *        arrays containing options with a single value
 * - 1.0: First commit
*/

#ifdef __cplusplus
extern "C" {
#endif

/*
 ********************************
 * Core Option Definitions
 ********************************
*/

/* RETRO_LANGUAGE_ENGLISH */

/* Default language:
 * - All other languages must include the same keys and values
 * - Will be used as a fallback in the event that frontend language
 *   is not available
 * - Will be used as a fallback for any missing entries in
 *   frontend language definition */


struct retro_core_option_v2_category option_cats_us[] = {
   {
      "video",
      "Video",
      "Configure visual buffers & effects, display parameters, framerate/-skip and rendering/texture parameters."
   },
   {
      "input",
      "Input",
      "Configure controller & light gun settings."
   },
   {
      "vmu",
      "Visual Memory",
      "Configure settings related to the Visual Memory Units/Systems (VMU)."
   },
   {
      "hacks",
      "Emulation Hacks",
      "Configure different emulation hacks."
   },
   { NULL, NULL, NULL },
};

struct retro_core_option_v2_definition option_defs_us[] = {
   {
      CORE_OPTION_NAME "_boot_to_bios",
      "Boot to BIOS (Restart Required)",
      NULL,
      "Boot directly into the Dreamcast BIOS menu.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_system",
      "System Type (Restart Required)",
      NULL,
      "",
      NULL,
      NULL,
      {
         { "auto",       "Auto" },
         { "dreamcast",  "Dreamcast" },
         { "naomi",      "NAOMI" },
         { "atomiswave", "Atomiswave" },
         { NULL, NULL },
      },
      "auto",
   },
   {
      CORE_OPTION_NAME "_use_real_bios",
      "Use Real BIOS (If Available)",
      NULL,
      "Start with the BIOS files in the system directory: the Dreamcast's, or naomi.zip (naomi2.zip) for a NAOMI game. When this is disabled, or the BIOS is not found, the built-in replacement (HLE) BIOS is used instead: it is less compatible, and for NAOMI it opens a game's own test menu but has no system menu of its own and keeps the default coin settings. 'Boot to BIOS' and Atomiswave games always need the real BIOS. Restart required.",
      NULL,
      NULL,
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL},
      },
      "enabled",
   },
   {
      CORE_OPTION_NAME "_internal_resolution",
      "Internal Resolution",
      NULL,
      "Modify rendering resolution: a multiple of the console's 640x480.",
      NULL,
      "video",
      {
         { "1x", NULL },
         { "2x", NULL },
         { "4x", NULL },
         { "8x", NULL },
         { NULL, NULL },
      },
      "1x",
   },
   {
      CORE_OPTION_NAME "_ssaa",
      "SSAA",
      NULL,
      "Supersampling: each pixel of the Internal Resolution drawn from 2, 4 or 8 samples, its edges smoothed. Sprites and the HUD stay as they are. Vulkan and Direct3D 12 only; fewer samples when the device or its video memory has not room for these. 8x is best with the Compute rendering method; with the rasterizer it is costly at high Internal Resolutions.",
      NULL,
      "video",
      {
         { "disabled", "Off" },
         { "2x", NULL },
         { "4x", NULL },
         { "8x", NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_screen_rotation",
      "Screen Orientation",
      NULL,
      "",
      NULL,
      "video",
      {
         { "horizontal", "Horizontal" },
         { "vertical",   "Vertical" },
         { NULL, NULL },
      },
      "horizontal",
   },
   {/* TODO: needs explanation */
      CORE_OPTION_NAME "_alpha_sorting",
      "Alpha Sorting",
      NULL,
      "",
      NULL,
      NULL,
      {
         { "per-strip (fast, least accurate)", "Per-Strip (fast, least accurate)" },
         { "per-triangle (normal)",            "Per-Triangle (normal)" },
#if defined(HAVE_GL4) || defined(HAVE_VULKAN)
         { "per-pixel (accurate)",             "Per-Pixel (accurate, but slowest)" },
#endif
         { NULL, NULL },
      },
#if defined(LOW_END)
      "per-strip (fast, least accurate)",
#else
      "per-triangle (normal)",
#endif
   },
   {
      CORE_OPTION_NAME "_gdrom_fast_loading",
      "GD-ROM Fast Loading (inaccurate)",
      NULL,
      "Speeds up GD-ROM loading.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
#ifdef LOW_END
      "enabled",
#else
      "disabled",
#endif
   },
   {
      CORE_OPTION_NAME "_force_texture_lod0",
      "Force Texture LOD0",
      NULL,
      "Disable this for traditional hardware mipmapping. Enabling this will bypass mipmapping and always use texture LOD0 instead. The result is better image quality.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_widescreen_hack",
      "Widescreen Hack",
      NULL,
      "",
      NULL,
      "hacks",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_widescreen_cheats",
      "Widescreen Cheats",
      NULL,
      "Activates cheats that allow certain games to display in widescreen format.",
      NULL,
      "hacks",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_cable_type",
      "Cable Type",
      NULL,
      "The output signal type. 'TV (Composite)' is the most widely supported.",
      NULL,
      "video",
      {
         { "TV (RGB)",       NULL },
         { "TV (Composite)", NULL },
         { "VGA (RGB)",      NULL },
         { NULL, NULL },
      },
#ifdef LOW_END
      "VGA (RGB)",
#else
      "TV (Composite)",
#endif
   },
   {
      CORE_OPTION_NAME "_broadcast",
      "Broadcast Standard",
      NULL,
      "",
      NULL,
      "video",
      {
         { "Default", NULL },
         { "PAL_M",   "PAL-M (Brazil)" },
         { "PAL_N",   "PAL-N (Argentina, Paraguay, Uruguay)" },
         { "NTSC",    NULL },
         { "PAL",     "PAL (World)" },
         { NULL, NULL },
      },
      "Default",
   },
#ifdef ENABLE_MODEM
   {
      CORE_OPTION_NAME "_network_device",
      "Expansion Port Device (Restart Required)",
      NULL,
      "The device in the Dreamcast's expansion port, for online games: the modem it came with, the Broadband Adapter (Ethernet), or none.",
      NULL,
      NULL,
      {
         { "modem", "Modem" },
         { "bba",   "Broadband Adapter" },
         { "none",  "None" },
         { NULL, NULL },
      },
      "modem",
   },
   {
      CORE_OPTION_NAME "_dcnet",
      "Use DCNet",
      NULL,
      "Go online through the DCNet relay (dcnet.flyca.st) instead of the built-in network stack. Taken at the next connection.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
#endif
   {
      CORE_OPTION_NAME "_region",
      "Region",
      NULL,
      "",
      NULL,
      NULL,
      {
         { "Default", NULL },
         { "Japan",   NULL },
         { "USA",     NULL },
         { "Europe",  NULL },
         { NULL, NULL },
      },
      "Default",
   },
   {
      CORE_OPTION_NAME "_language",
      "Language",
      NULL,
      "",
      NULL,
      NULL,
      {
         { "Default",  NULL },
         { "Japanese", NULL },
         { "English",  NULL },
         { "German",   NULL },
         { "French",   NULL },
         { "Spanish",  NULL },
         { "Italian",  NULL },
         { NULL, NULL },
      },
      "Default",
   },
   {
      CORE_OPTION_NAME "_div_matching",
      "DIV Matching",
      NULL,
      "Optimize integer division",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "auto",     "Auto" },
         { NULL, NULL },
      },
      "auto",
   },
   {
      CORE_OPTION_NAME "_sh4_timing",
      "SH4 Timing (Restart)",
      NULL,
      "How long each instruction of the main processor is taken to last. 'Legacy' is the rule of thumb this core has always used: every integer instruction 8 cycles and every floating-point one none, with some kinds of loop hurried along. 'Accurate' counts them as the processor issues them, which makes the processor about three times as fast as 'Legacy' has it, and is what upstream Flycast does. Try 'Accurate' for a game whose speed or timing is off; it takes more of the host. A NAOMI 2 game always runs with 'Accurate'.",
      NULL,
      NULL,
      {
         { "legacy",   "Legacy" },
         { "accurate", "Accurate" },
         { NULL, NULL },
      },
      "legacy",
   },
#ifdef FEAT_AREC_SELECTABLE
   {
      CORE_OPTION_NAME "_arm7_recompiler",
      "Sound CPU Recompiler",
      NULL,
      "Run the sound chip's ARM7 processor through the recompiler. Disable to run it through the slower interpreter instead, to find out whether a sound problem comes from the recompiler. Either way it runs the same on x86-64, 32-bit x86 and ARM64. Takes effect at once.",
      NULL,
      NULL,
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled",
   },
#endif
   {
      CORE_OPTION_NAME "_force_wince",
      "Force Windows CE Mode",
      NULL,
      "Enable full MMU (Memory Management Unit) emulation and other settings for Windows CE games.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_analog_stick_deadzone",
      "Analog Stick Deadzone",
      NULL,
      "",
      NULL,
      "input",
      {
         { "0%",  NULL },
         { "5%",  NULL },
         { "10%", NULL },
         { "15%", NULL },
         { "20%", NULL },
         { "25%", NULL },
         { "30%", NULL },
         { NULL, NULL },
      },
      "15%",
   },
   {
      CORE_OPTION_NAME "_trigger_deadzone",
      "Trigger Deadzone",
      NULL,
      "",
      NULL,
      "input",
      {
         { "0%",  NULL },
         { "5%",  NULL },
         { "10%", NULL },
         { "15%", NULL },
         { "20%", NULL },
         { "25%", NULL },
         { "30%", NULL },
         { NULL, NULL },
      },
      "0%",
   },
   {
      CORE_OPTION_NAME "_digital_triggers",
      "Digital Triggers",
      NULL,
      "",
      NULL,
      "input",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_precompile_fpcb",
      "Reduce Recompiler Stalls (High Memory)",
      NULL,
      "Commit the recompiler's block-dispatch table up front instead of on first use. Reduces frame-time stalls the first time a game reaches new code (boot, level loads), at the cost of extra memory (up to 64MB, or 128MB with expanded RAM). Leave disabled unless you see hitching on new areas and have memory to spare.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_anisotropic_filtering",
      "Anisotropic Filtering",
      NULL,
      "Enhance the quality of textures on surfaces that are at oblique viewing angles with respect to the camera.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "2",  NULL },
         { "4",  NULL },
         { "8",  NULL },
         { "16",  NULL },
         { NULL, NULL },
      },
      "4",
   },
   {
      CORE_OPTION_NAME "_gpu_texture_decode",
      "GPU Texture Decoding",
      NULL,
      "Convert textures from the Dreamcast's formats on the GPU instead of the CPU, from their bytes in video memory as they are. The textures are the same either way; this takes the work off the CPU. Vulkan, Direct3D 11 and 12, and OpenGL 3 and later; Direct3D 9 only with 'Enabled, Direct3D 9 too'. Textures that are upscaled or dumped are always converted on the CPU. Planar 16-bit textures go to the GPU as they are on every renderer, whatever this says.",
      NULL,
      "video",
      {
         { "enabled",  NULL },
         { "enabled_d3d9", "Enabled, Direct3D 9 too" },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled",
   },
#if defined(HAVE_VULKAN) || defined(HAVE_GL4) || defined(HAVE_D3D12)
   {
      CORE_OPTION_NAME "_rendering_method",
      "Rendering Method",
      NULL,
      "How the screen's frames are drawn. Rasterizer draws them with the GPU's rasterizer. Compute draws them tile by tile with compute shaders, the way the Dreamcast does, translucent polygons sorted per pixel with none left out; frames it cannot draw yet (renders to textures) are drawn by the per-pixel rasterizer. Vulkan and Direct3D 12 only (Direct3D 12: shader model 6.0 with 64-bit integers); experimental.",
      NULL,
      "video",
      {
         { "rasterizer", "Rasterizer" },
         { "compute", "Compute" },
         { NULL, NULL },
      },
      "rasterizer",
   },
   {
      CORE_OPTION_NAME "_ta_processing",
      "TA Processing",
      NULL,
      "Where the lists the game sends the tile accelerator are made into vertices and indices. GPU does it with compute shaders, straight from the data as the game sent it, into the GPU's own buffers, the same to the byte, and the frame is drawn from them. Vulkan, Direct3D 12, OpenGL 4.3 or later with persistently mapped buffers (4.4, or ARB_buffer_storage), and Direct3D 11 on NVIDIA GPUs only (through NVIDIA's NVAPI driver extensions; with other GPUs Direct3D 11 stays on the CPU).",
      NULL,
      "video",
      {
         { "cpu", "CPU" },
         { "gpu", "GPU" },
         { NULL, NULL },
      },
      "cpu",
   },
   {
      CORE_OPTION_NAME "_color_format",
      "Color Format",
      NULL,
      "The output's colour depth. '24-bit (Standard)' is the picture as it always was, 8 bits a channel. '30-bit Color (HDR)' hands the frontend 10-bit PQ Rec.2020 (HDR10) for an HDR display, and '64-bit Color (HDR, scRGB)' 16-bit floating point linear Rec.709: the picture is placed at the frontend's paper white and mapped onto the display through the Display Transform below. The Dreamcast's own picture is standard range, so what goes above paper white is what HDR Expansion lifts there. scRGB falls back to 30-bit when the frontend does not take it, and both fall back to 24-bit when the frontend cannot present HDR. Vulkan, Direct3D 11 (feature level 11.0) and Direct3D 12 only. Restart required.",
      NULL,
      "video",
      {
         { "24bit",       "24-bit (Standard)" },
         { "30bit_hdr",   "30-bit Color (HDR)" },
         { "64bit_scrgb", "64-bit Color (HDR, scRGB)" },
         { NULL, NULL },
      },
      "24bit"
   },
   {
      CORE_OPTION_NAME "_hdr_display_transform",
      "Display Transform (Tone Curve)",
      NULL,
      "How the picture is mapped onto the HDR display. 'Reinhard (Soft Knee)' is the reference: everything up to paper white is the 24-bit picture exactly, and only what HDR Expansion lifts above it eases toward the display's peak. 'Exponential Knee' is the same knee with a faster shoulder. The filmic curves reshape the whole range: ACES, Hejl and Unreal lift mid-tones for a brighter filmic look, GT and Hable stay close to the reference, Lottes is darker and contrastier. Each keeps paper white at paper white. 'ACES 2.0 Tone Scale' is that curve alone, per channel; 'ACES 2.0 (Full Output Transform)' is the whole transform - appearance model, chroma and gamut compression - which desaturates bright colours toward white instead of letting one channel clip. AgX and Khronos PBR Neutral likewise work on the whole colour. Only with Color Format set to 30-bit or 64-bit, on the Vulkan, Direct3D 11 and Direct3D 12 renderers.",
      NULL,
      "video",
      {
         { "reinhard",  "Reinhard (Soft Knee)" },
         { "knee_exp",  "Exponential Knee" },
         { "rplain",    "Reinhard (Plain)" },
         { "rext",      "Reinhard (Extended, White 4)" },
         { "jodie",     "Reinhard-Jodie (Hue-Preserving)" },
         { "devlin",    "Reinhard-Devlin (Classic)" },
         { "aces",      "ACES (Narkowicz)" },
         { "acesfit",   "ACES Fitted (Hill)" },
         { "aces2",     "ACES 2.0 Tone Scale" },
         { "aces2full", "ACES 2.0 (Full Output Transform)" },
         { "hejl",      "Hejl (Filmic)" },
         { "filmicalu", "Filmic ALU (Hejl 2015)" },
         { "filmiclog", "Filmic (Sobotka)" },
         { "hable",     "Hable (Uncharted 2)" },
         { "hable2017", "Hable (2017 Piecewise)" },
         { "lottes",    "Lottes (AMD)" },
         { "gt",        "GT (Uchimura)" },
         { "unreal",    "Unreal" },
         { "neutral",   "Khronos PBR Neutral" },
         { "agx",       "AgX" },
         { "drago",     "Drago (Logarithmic)" },
         { "expo",      "Exponential" },
         { "tumblin",   "Tumblin-Rushmeier (Classic)" },
         { "ward",      "Ward (Classic, Linear)" },
         { "schlick",   "Schlick (Classic)" },
         { NULL, NULL },
      },
      "reinhard"
   },
   {
      CORE_OPTION_NAME "_hdr_expansion",
      "HDR Expansion",
      NULL,
      "The Dreamcast's picture is standard range: without expansion nothing in it rises above paper white. Expansion lifts the top of the range into the HDR headroom so bright areas read brighter than white; mid-tones below the knee never move. 'Inverse Tonemap' expands each channel on its own and over-saturates bright colours; 'Hue-Preserving' expands the brightest channel and scales the others with it, so a highlight brightens along its own colour. With every Display Transform but the two knees. Only with Color Format set to 30-bit or 64-bit, on the Vulkan, Direct3D 11 and Direct3D 12 renderers.",
      NULL,
      "video",
      {
         { "disabled", "None" },
         { "inverse",  "Inverse Tonemap (Basic Expansion)" },
         { "hue",      "Hue-Preserving Expansion" },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      CORE_OPTION_NAME "_hdr_peak",
      "HDR Display Peak Luminance",
      NULL,
      "The peak brightness every Display Transform carries highlights up to, in place of the one the frontend reports. 'Display' takes the panel's own reported peak, where the frontend knows it. ACES 2.0 (Full) solves its tables again for a new peak. Leave on Auto unless the frontend reports nothing or the display's real peak differs from what it claims. Only with Color Format set to 30-bit or 64-bit, on the Vulkan, Direct3D 11 and Direct3D 12 renderers.",
      NULL,
      "video",
      {
         { "auto",    "Auto (ask the frontend)" },
         { "display", "Display (the panel's reported peak)" },
         { "400",     "400 nits" },
         { "600",     "600 nits" },
         { "1000",    "1000 nits" },
         { "1500",    "1500 nits" },
         { "2000",    "2000 nits" },
         { "4000",    "4000 nits" },
         { NULL, NULL },
      },
      "auto"
   },
   {
      CORE_OPTION_NAME "_hdr_highlight_hue",
      "Highlight Hue Preservation",
      NULL,
      "Keeps the hue of saturated highlights under the per-channel Display Transform curves: a bright colour is mixed toward an achromatic peak by however much that peak was compressed, so it desaturates along its own hue instead of sliding toward whichever channel clips last. ACES 2.0 (Full), AgX and Khronos PBR Neutral already do this. Only with Color Format set to 30-bit or 64-bit, on the Vulkan, Direct3D 11 and Direct3D 12 renderers.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      CORE_OPTION_NAME "_hdr_sdr_gamma",
      "HDR Reference Display Gamma",
      NULL,
      "The transfer the picture is decoded with before it is placed in the HDR range. '2.4 (BT.1886)' matches RetroArch's own SDR-to-HDR composition and a TV; '2.2' a PC monitor; 'sRGB' what Windows assumes for SDR content on an HDR desktop, which lifts the shadows. All three agree at black and at paper white. Only with Color Format set to 30-bit or 64-bit, on the Vulkan, Direct3D 11 and Direct3D 12 renderers.",
      NULL,
      "video",
      {
         { "2.4",  "2.4 (BT.1886)" },
         { "2.2",  "2.2" },
         { "srgb", "sRGB" },
         { NULL, NULL },
      },
      "2.4"
   },
   {
      CORE_OPTION_NAME "_hdr_aces2_surround",
      "ACES 2.0: Surround",
      NULL,
      "The viewing-environment compensation of the full ACES 2.0 transform: dim is the reference, dark suits a room with the lights off, average a bright one. Only with 'ACES 2.0 (Full Output Transform)'.",
      NULL,
      "video",
      {
         { "dark",    NULL },
         { "dim",     NULL },
         { "average", NULL },
         { NULL, NULL },
      },
      "dim"
   },
   {
      CORE_OPTION_NAME "_hdr_aces2_gamut",
      "ACES 2.0: Limiting Gamut",
      NULL,
      "The gamut the full ACES 2.0 transform compresses colour toward, which should be the one the display has. 'Auto' takes the display's own primaries when the frontend reports them, else P3-D65, which most HDR displays are close to. Only with 'ACES 2.0 (Full Output Transform)'.",
      NULL,
      "video",
      {
         { "auto",    "Auto (the display's, else P3-D65)" },
         { "rec2020", "Rec.2020 (widest)" },
         { "p3",      "P3-D65 (most HDR displays)" },
         { "rec709",  "Rec.709 (SDR gamut)" },
         { NULL, NULL },
      },
      "auto"
   },
   {
      CORE_OPTION_NAME "_hdr_agx_look",
      "AgX: Look",
      NULL,
      "A grade on the AgX Display Transform's output. Punchy is the official higher-contrast, higher-saturation look (contrast 1.35, saturation 1.4). Only with AgX.",
      NULL,
      "video",
      {
         { "default", NULL },
         { "punchy",  NULL },
         { NULL, NULL },
      },
      "default"
   },
   {
      CORE_OPTION_NAME "_hdr_gt_toe",
      "GT: Toe",
      NULL,
      "Where the GT (Uchimura) curve's linear section starts; a larger value gives a longer, softer approach out of black. Only with GT.",
      NULL,
      "video",
      {
         { "0.10", "0.10 (short toe)" },
         { "0.16", NULL },
         { "0.22", "0.22 (default)" },
         { "0.30", NULL },
         { "0.40", "0.40 (long toe)" },
         { NULL, NULL },
      },
      "0.22"
   },
   {
      CORE_OPTION_NAME "_hdr_gt_linear",
      "GT: Linear Length",
      NULL,
      "How long the GT (Uchimura) curve's straight middle section runs before its shoulder begins. Only with GT.",
      NULL,
      "video",
      {
         { "0.20", "0.20 (early shoulder)" },
         { "0.30", NULL },
         { "0.40", "0.40 (default)" },
         { "0.50", NULL },
         { "0.70", "0.70 (late shoulder)" },
         { NULL, NULL },
      },
      "0.40"
   },
   {
      CORE_OPTION_NAME "_hdr_filmiclog_contrast",
      "Filmic: Contrast",
      NULL,
      "Which of Troy Sobotka's seven Filmic contrast curves 'Filmic (Sobotka)' uses, from lifted shadows to crushed ones. Only with Filmic (Sobotka).",
      NULL,
      "video",
      {
         { "verylow",  "Very Low (most shadow detail)" },
         { "low",      "Low" },
         { "medium",   "Medium" },
         { "base",     "Base (default)" },
         { "medhigh",  "Medium High" },
         { "high",     "High" },
         { "veryhigh", "Very High (most crushed)" },
         { NULL, NULL },
      },
      "base"
   },
   {
      CORE_OPTION_NAME "_hdr_filmiclog_range",
      "Filmic: Highlight Range",
      NULL,
      "How many stops above mid grey the 'Filmic (Sobotka)' log encode reaches before it saturates. Only with Filmic (Sobotka).",
      NULL,
      "video",
      {
         { "4",  "4 stops (Blender default)" },
         { "6",  "6 stops" },
         { "8",  "8 stops" },
         { "10", "10 stops" },
         { NULL, NULL },
      },
      "4"
   },
   {
      CORE_OPTION_NAME "_hdr_bloom",
      "HDR Bloom",
      NULL,
      "Multi-band bloom for the HDR output. Bright content is extracted in linear light, blurred at two scales - a tight core and a wide haze - and added before the Display Transform, so the glow genuinely exceeds paper white and lands in the HDR headroom. Firefly-compressed at extraction so a single bright texel cannot make the halo flicker. 'Disabled' leaves the picture as before. Only with Color Format set to 30-bit or 64-bit, on the Vulkan, Direct3D 11 and Direct3D 12 renderers.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "subtle",   "Subtle" },
         { "standard", "Standard" },
         { "intense",  "Intense" },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      CORE_OPTION_NAME "_hdr_bloom_select",
      "HDR Bloom: Selection",
      NULL,
      "What counts as a highlight for 'HDR Bloom'. 'Classic' keeps the 0.70 threshold the bloom was tuned against. 'Curve Knee' derives it from the active Display Transform: Neutral's 0.76 start of compression, or GT's solved shoulder start, so the bloom picks exactly the pixels the curve will compress. Other curves keep 0.70.",
      NULL,
      "video",
      {
         { "classic", "Classic" },
         { "knee",    "Curve Knee" },
         { NULL, NULL },
      },
      "classic"
   },
   {
      CORE_OPTION_NAME "_hdr_bloom_range",
      "HDR Bloom: Range",
      NULL,
      "How much brightness the bloom extraction may carry. The bright pass limits what it extracts so one hot texel cannot flicker the halo; 'Standard' caps every extraction below 1.0, so very bright and moderately bright sources bloom alike. 'Wide' and 'Widest' keep the anti-flicker property but let a brighter source bloom brighter. Shows most with HDR Expansion and a curve with headroom, such as ACES 2.0 or Filmic.",
      NULL,
      "video",
      {
         { "standard", "Standard (clamped)" },
         { "wide",     "Wide (4x)" },
         { "widest",   "Widest (16x)" },
         { NULL, NULL },
      },
      "standard"
   },
   {
      CORE_OPTION_NAME "_hdr_bloom_convolution",
      "HDR Bloom: Convolution",
      NULL,
      "Replaces the bloom's two fixed blur scales with a six-octave pyramid accumulated back up with a tent filter - an approximation of one wide, heavy-tailed point spread. Glow carries much further and fades out smoothly instead of stopping at the blur kernel's edge. Total bloom energy is the same, so cores are softer and spill wider. Needs 'HDR Bloom'.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      CORE_OPTION_NAME "_hdr_halation",
      "HDR Halation",
      NULL,
      "Warms the bloom around bright areas toward red-orange, the way film does: light passing through the emulsion scatters off the base and comes back reddened. Tints the existing bloom rather than adding a pass, so it needs 'HDR Bloom'. Pairs naturally with the Filmic Display Transform but is not tied to it.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "subtle",   "Subtle" },
         { "strong",   "Strong" },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      CORE_OPTION_NAME "_hdr_chromatic",
      "HDR Chromatic Aberration",
      NULL,
      "Splits red and blue slightly apart toward the edges of the frame, as a lens does when it focuses different wavelengths at different magnifications. Grows with the square of the distance from the centre, so the middle stays clean and only the corners fringe. The offset is a fixed fraction of the frame, so it looks the same at every internal resolution. Only with Color Format set to 30-bit or 64-bit, on the Vulkan, Direct3D 11 and Direct3D 12 renderers.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "subtle",   "Subtle" },
         { "strong",   "Strong" },
         { "heavy",    "Heavy" },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      CORE_OPTION_NAME "_hdr_post_compact",
      "HDR Post: Compact Intermediates",
      NULL,
      "Stores the images the HDR post effects work on - the full-size scene and the bloom chain - as packed 32-bit floats (11/11/10 bits) instead of 64-bit half floats, halving the memory traffic of the effects. Worth it on integrated and mobile GPUs, where that traffic is a real share of the frame. The picture changes slightly: a few 10-bit HDR output steps at most. Only matters while HDR Bloom or HDR Chromatic Aberration is on.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
#endif
   {
      CORE_OPTION_NAME "_pvr2_scaler",
      "PowerVR2 scaler, flicker filter and dither",
      NULL,
      "What the PowerVR2 does to the picture as it writes it to video memory. With the game's vertical scale factor above 1 (most of Sega's games, VGA too), each line blended with the lines above and below by the game's coefficients: the flicker filter, which softens the picture vertically. With a framebuffer of 16 bits a pixel (most games), the colours cut to its bits and dithered as the game asks, the bands and the dither pattern of the console's picture. With the game's horizontal scaling (rare), pairs of pixels averaged into one: the compute renderer only. Compute renderer (Vulkan, Direct3D 12) and the rasterizers of Vulkan, Direct3D 12, Direct3D 11 (where the GPU can read its picture back in a compute shader) and OpenGL 4.3 and later; not OpenGL ES, OpenGL 3 or Direct3D 9. Disabled draws the picture without them, as before.",
      NULL,
      "video",
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "disabled",
   },
#ifdef HAVE_TEXUPSCALE
   {
      CORE_OPTION_NAME "_texupscale",
      "Texture Upscaling (xBRZ)",
      NULL,
      "Enhance hand-drawn 2D pixel art graphics. Should only be used with 2D pixelated games.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "2x",  NULL },
         { "4x",  NULL },
         { "6x",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {/* TODO: needs clarification */
      CORE_OPTION_NAME "_texupscale_max_filtered_texture_size",
      "Texture Upscaling Max. Filtered Size",
      NULL,
      "",
      NULL,
      "video",
      {
         { "256",  NULL },
         { "512",  NULL },
         { "1024", NULL },
         { NULL, NULL },
      },
      "256",
   },
#endif
   {
      CORE_OPTION_NAME "_render_to_texture_upscaling",
      "Render To Texture Upscaling",
      NULL,
      "",
      NULL,
      "video",
      {
         { "1x", NULL },
         { "2x", NULL },
         { "3x", NULL },
         { "4x", NULL },
         { "8x", NULL },
         { NULL, NULL },
      },
      "1x",
   },
#if !defined(TARGET_NO_THREADS)
   {
      CORE_OPTION_NAME "_threaded_rendering",
      "Threaded Rendering",
      NULL,
      "Runs the GPU and CPU on different threads. Highly recommended.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled",
   },
#endif
   {
      CORE_OPTION_NAME "_frame_skipping",
      "Frame Skipping",
      NULL,
      "Sets the number of frames to skip between each displayed frame.",
      NULL,
      "video",
      {
         { "disabled",  NULL },
         { "1",         NULL },
         { "2",         NULL },
         { "3",         NULL },
         { "4",         NULL },
         { "5",         NULL },
         { "6",         NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_ta_header_check",
      "TA Header Pass Check (Debug)",
      NULL,
      "For development: puts every frame through the header pass of GPU-side TA processing as well as the usual parse, logs any difference between the two and the time each takes. Slower when on.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_enable_purupuru",
      "Purupuru Pack/Vibration Pack",
      NULL,
      "Enables controller force feedback.",
      NULL,
      "input",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled",
   },
   {
      CORE_OPTION_NAME "_allow_service_buttons",
      "Allow NAOMI Service Buttons",
      NULL,
      "Enables SERVICE button for NAOMI, to enter cabinet settings.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_enable_naomi_15khz_dipswitch",
      "Enable NAOMI 15KHz DIP switch",
      NULL,
      "This can force display in 240p, 480i or have no effect at all depending on the game.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_naomi_card_readers",
      "NAOMI Card Readers",
      NULL,
      "Plug in the card reader of a game that has one: Initial D, Club Kart, Derby Owners Club, Mushiking, Virtua Fighter 4. L inserts a card. Cards are kept in the save directory. Takes effect when the game is next started or hard reset.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_naomi_wheel_ffb",
      "NAOMI Wheel Force Feedback",
      NULL,
      "Rumble the first controller with the steering wheel's force feedback: Initial D, Club Kart, The King of Route 66, F355 Challenge, Tokyo Bus Guide, 18 Wheeler.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {/* TODO: needs explanation */
      CORE_OPTION_NAME "_custom_textures",
      "Load Custom Textures",
      NULL,
      "",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {/* TODO: probably needs explanation */
      CORE_OPTION_NAME "_dump_textures",
      "Dump Textures",
      NULL,
      "",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_texture_debug",
      "Texture Debug Log (Vulkan)",
      NULL,
      "For finding faults in how textures are drawn. Every 10 seconds, writes to the log a line for each texture the last frame drew: its format, size, levels, how it was made and the sampler it is drawn with, and each level read back from the GPU with a checksum, beside the checksum of what was uploaded. Slow while it reads back; leave it disabled.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_per_content_vmus",
      "Per-Game Visual Memory Units/Systems (VMU)",
      "Per-Game VMUs",
      "When disabled, all games share 4 VMU save files (A1, B1, C1, D1) located in RetroArch's system directory. The 'VMU A1' setting creates a unique VMU 'A1' file in RetroArch's save directory for each game that is launched. The 'All VMUs' setting creates 4 unique VMU files (A1, B1, C1, D1) for each game that is launched.",
      NULL,
      "vmu",
      {
         { "disabled", NULL },
         { "VMU A1",   NULL },
         { "All VMUs", NULL },
         { NULL, NULL},
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_vmu_sound",
      "VMU Sounds",
      NULL,
      "Play the beeps games make on the Visual Memory Units.",
      NULL,
      "vmu",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_show_vmu_screen_settings",
      "Show Visual Memory Unit/System (VMU) Display Settings",
      "Show VMU Display Settings",
      "Enable configuration of emulated VMU LCD screen visibility, size, position and color. NOTE: Quick Menu must be toggled for this setting to take effect.",
      NULL,
      "vmu",
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL},
      },
      "disabled"
   },
   {
      CORE_OPTION_NAME "_vmu1_screen_display",
      "VMU Screen 1 Display",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_vmu1_screen_position",
      "VMU Screen 1 Position",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "Upper Left",  NULL },
         { "Upper Right", NULL },
         { "Lower Left",  NULL },
         { "Lower Right", NULL },
         { NULL, NULL },
      },
      "Upper Left",
   },
   {
      CORE_OPTION_NAME "_vmu1_screen_size_mult",
      "VMU Screen 1 Size",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "1x", NULL },
         { "2x", NULL },
         { "3x", NULL },
         { "4x", NULL },
         { "5x", NULL },
         { NULL, NULL },
      },
      "1x",
   },
   {
      CORE_OPTION_NAME "_vmu1_pixel_on_color",
      "VMU Screen 1 Pixel On Color",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "DEFAULT_ON 00",  "Default ON" },
         { "DEFAULT_OFF 01", "Default OFF" },
         { "BLACK 02",          "Black" },
         { "BLUE 03",           "Blue" },
         { "LIGHT_BLUE 04",     "Light Blue" },
         { "GREEN 05",          "Green" },
         { "CYAN 06",           "Cyan" },
         { "CYAN_BLUE 07",      "Cyan Blue" },
         { "LIGHT_GREEN 08",    "Light Green" },
         { "CYAN_GREEN 09",     "Cyan Green" },
         { "LIGHT_CYAN 10",     "Light Cyan" },
         { "RED 11",            "Red" },
         { "PURPLE 12",         "Purple" },
         { "LIGHT_PURPLE 13",   "Light Purple" },
         { "YELLOW 14",         "Yellow" },
         { "GRAY 15",           "Gray" },
         { "LIGHT_PURPLE_2 16", "Light Purple (2)" },
         { "LIGHT_GREEN_2 17",  "Light Green (2)" },
         { "LIGHT_GREEN_3 18",  "Light Green (3)" },
         { "LIGHT_CYAN_2 19",   "Light Cyan (2)" },
         { "LIGHT_RED_2 20",    "Light Red (2)" },
         { "MAGENTA 21",        "Magenta" },
         { "LIGHT_PURPLE_3 22",   "Light Purple (3)" },
         { "LIGHT_ORANGE 23",   "Light Orange" },
         { "ORANGE 24",         "Orange" },
         { "LIGHT_PURPLE_4 25", "Light Purple (4)" },
         { "LIGHT_YELLOW 26",   "Light Yellow" },
         { "LIGHT_YELLOW_2 27", "Light Yellow (2)" },
         { "WHITE 28",          "White" },
         { NULL, NULL },
      },
      "DEFAULT_ON 00",
   },
   {
      CORE_OPTION_NAME "_vmu1_pixel_off_color",
      "VMU Screen 1 Pixel Off Color",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "DEFAULT_OFF 01", "Default OFF" },
         { "DEFAULT_ON 00",  "Default ON" },
         { "BLACK 02",          "Black" },
         { "BLUE 03",           "Blue" },
         { "LIGHT_BLUE 04",     "Light Blue" },
         { "GREEN 05",          "Green" },
         { "CYAN 06",           "Cyan" },
         { "CYAN_BLUE 07",      "Cyan Blue" },
         { "LIGHT_GREEN 08",    "Light Green" },
         { "CYAN_GREEN 09",     "Cyan Green" },
         { "LIGHT_CYAN 10",     "Light Cyan" },
         { "RED 11",            "Red" },
         { "PURPLE 12",         "Purple" },
         { "LIGHT_PURPLE 13",   "Light Purple" },
         { "YELLOW 14",         "Yellow" },
         { "GRAY 15",           "Gray" },
         { "LIGHT_PURPLE_2 16", "Light Purple (2)" },
         { "LIGHT_GREEN_2 17",  "Light Green (2)" },
         { "LIGHT_GREEN_3 18",  "Light Green (3)" },
         { "LIGHT_CYAN_2 19",   "Light Cyan (2)" },
         { "LIGHT_RED_2 20",    "Light Red (2)" },
         { "MAGENTA 21",        "Magenta" },
         { "LIGHT_PURPLE_3 22",   "Light Purple (3)" },
         { "LIGHT_ORANGE 23",   "Light Orange" },
         { "ORANGE 24",         "Orange" },
         { "LIGHT_PURPLE_4 25", "Light Purple (4)" },
         { "LIGHT_YELLOW 26",   "Light Yellow" },
         { "LIGHT_YELLOW_2 27", "Light Yellow (2)" },
         { "WHITE 28",          "White" },
         { NULL, NULL },
      },
      "DEFAULT_OFF 01",
   },
   {
      CORE_OPTION_NAME "_vmu1_screen_opacity",
      "VMU Screen 1 Opacity",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "10%",  NULL },
         { "20%",  NULL },
         { "30%",  NULL },
         { "40%",  NULL },
         { "50%",  NULL },
         { "60%",  NULL },
         { "70%",  NULL },
         { "80%",  NULL },
         { "90%",  NULL },
         { "100%", NULL },
         { NULL,   NULL },
      },
      "100%",
   },
   {
      CORE_OPTION_NAME "_vmu2_screen_display",
      "VMU Screen 2 Display",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_vmu2_screen_position",
      "VMU Screen 2 Position",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "Upper Left",  NULL },
         { "Upper Right", NULL },
         { "Lower Left",  NULL },
         { "Lower Right", NULL },
         { NULL, NULL },
      },
      "Upper Left",
   },
   {
      CORE_OPTION_NAME "_vmu2_screen_size_mult",
      "VMU Screen 2 Size",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "1x", NULL },
         { "2x", NULL },
         { "3x", NULL },
         { "4x", NULL },
         { "5x", NULL },
         { NULL, NULL },
      },
      "1x",
   },
   {
      CORE_OPTION_NAME "_vmu2_pixel_on_color",
      "VMU Screen 2 Pixel On Color",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "DEFAULT_ON 00",  "Default ON" },
         { "DEFAULT_OFF 01", "Default OFF" },
         { "BLACK 02",          "Black" },
         { "BLUE 03",           "Blue" },
         { "LIGHT_BLUE 04",     "Light Blue" },
         { "GREEN 05",          "Green" },
         { "CYAN 06",           "Cyan" },
         { "CYAN_BLUE 07",      "Cyan Blue" },
         { "LIGHT_GREEN 08",    "Light Green" },
         { "CYAN_GREEN 09",     "Cyan Green" },
         { "LIGHT_CYAN 10",     "Light Cyan" },
         { "RED 11",            "Red" },
         { "PURPLE 12",         "Purple" },
         { "LIGHT_PURPLE 13",   "Light Purple" },
         { "YELLOW 14",         "Yellow" },
         { "GRAY 15",           "Gray" },
         { "LIGHT_PURPLE_2 16", "Light Purple (2)" },
         { "LIGHT_GREEN_2 17",  "Light Green (2)" },
         { "LIGHT_GREEN_3 18",  "Light Green (3)" },
         { "LIGHT_CYAN_2 19",   "Light Cyan (2)" },
         { "LIGHT_RED_2 20",    "Light Red (2)" },
         { "MAGENTA 21",        "Magenta" },
         { "LIGHT_PURPLE_3 22",   "Light Purple (3)" },
         { "LIGHT_ORANGE 23",   "Light Orange" },
         { "ORANGE 24",         "Orange" },
         { "LIGHT_PURPLE_4 25", "Light Purple (4)" },
         { "LIGHT_YELLOW 26",   "Light Yellow" },
         { "LIGHT_YELLOW_2 27", "Light Yellow (2)" },
         { "WHITE 28",          "White" },
         { NULL, NULL },
      },
      "DEFAULT_ON 00",
   },
   {
      CORE_OPTION_NAME "_vmu2_pixel_off_color",
      "VMU Screen 2 Pixel Off Color",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "DEFAULT_OFF 01", "Default OFF" },
         { "DEFAULT_ON 00",  "Default ON" },
         { "BLACK 02",          "Black" },
         { "BLUE 03",           "Blue" },
         { "LIGHT_BLUE 04",     "Light Blue" },
         { "GREEN 05",          "Green" },
         { "CYAN 06",           "Cyan" },
         { "CYAN_BLUE 07",      "Cyan Blue" },
         { "LIGHT_GREEN 08",    "Light Green" },
         { "CYAN_GREEN 09",     "Cyan Green" },
         { "LIGHT_CYAN 10",     "Light Cyan" },
         { "RED 11",            "Red" },
         { "PURPLE 12",         "Purple" },
         { "LIGHT_PURPLE 13",   "Light Purple" },
         { "YELLOW 14",         "Yellow" },
         { "GRAY 15",           "Gray" },
         { "LIGHT_PURPLE_2 16", "Light Purple (2)" },
         { "LIGHT_GREEN_2 17",  "Light Green (2)" },
         { "LIGHT_GREEN_3 18",  "Light Green (3)" },
         { "LIGHT_CYAN_2 19",   "Light Cyan (2)" },
         { "LIGHT_RED_2 20",    "Light Red (2)" },
         { "MAGENTA 21",        "Magenta" },
         { "LIGHT_PURPLE_3 22",   "Light Purple (3)" },
         { "LIGHT_ORANGE 23",   "Light Orange" },
         { "ORANGE 24",         "Orange" },
         { "LIGHT_PURPLE_4 25", "Light Purple (4)" },
         { "LIGHT_YELLOW 26",   "Light Yellow" },
         { "LIGHT_YELLOW_2 27", "Light Yellow (2)" },
         { "WHITE 28",          "White" },
         { NULL, NULL },
      },
      "DEFAULT_OFF 01",
   },
   {
      CORE_OPTION_NAME "_vmu2_screen_opacity",
      "VMU Screen 2 Opacity",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "10%",  NULL },
         { "20%",  NULL },
         { "30%",  NULL },
         { "40%",  NULL },
         { "50%",  NULL },
         { "60%",  NULL },
         { "70%",  NULL },
         { "80%",  NULL },
         { "90%",  NULL },
         { "100%", NULL },
         { NULL,   NULL },
      },
      "100%",
   },
   {
      CORE_OPTION_NAME "_vmu3_screen_display",
      "VMU Screen 3 Display",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_vmu3_screen_position",
      "VMU Screen 3 Position",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "Upper Left",  NULL },
         { "Upper Right", NULL },
         { "Lower Left",  NULL },
         { "Lower Right", NULL },
         { NULL, NULL },
      },
      "Upper Left",
   },
   {
      CORE_OPTION_NAME "_vmu3_screen_size_mult",
      "VMU Screen 3 Size",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "1x", NULL },
         { "2x", NULL },
         { "3x", NULL },
         { "4x", NULL },
         { "5x", NULL },
         { NULL, NULL },
      },
      "1x",
   },
   {
      CORE_OPTION_NAME "_vmu3_pixel_on_color",
      "VMU Screen 3 Pixel On Color",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "DEFAULT_ON 00",  "Default ON" },
         { "DEFAULT_OFF 01", "Default OFF" },
         { "BLACK 02",          "Black" },
         { "BLUE 03",           "Blue" },
         { "LIGHT_BLUE 04",     "Light Blue" },
         { "GREEN 05",          "Green" },
         { "CYAN 06",           "Cyan" },
         { "CYAN_BLUE 07",      "Cyan Blue" },
         { "LIGHT_GREEN 08",    "Light Green" },
         { "CYAN_GREEN 09",     "Cyan Green" },
         { "LIGHT_CYAN 10",     "Light Cyan" },
         { "RED 11",            "Red" },
         { "PURPLE 12",         "Purple" },
         { "LIGHT_PURPLE 13",   "Light Purple" },
         { "YELLOW 14",         "Yellow" },
         { "GRAY 15",           "Gray" },
         { "LIGHT_PURPLE_2 16", "Light Purple (2)" },
         { "LIGHT_GREEN_2 17",  "Light Green (2)" },
         { "LIGHT_GREEN_3 18",  "Light Green (3)" },
         { "LIGHT_CYAN_2 19",   "Light Cyan (2)" },
         { "LIGHT_RED_2 20",    "Light Red (2)" },
         { "MAGENTA 21",        "Magenta" },
         { "LIGHT_PURPLE_3 22",   "Light Purple (3)" },
         { "LIGHT_ORANGE 23",   "Light Orange" },
         { "ORANGE 24",         "Orange" },
         { "LIGHT_PURPLE_4 25", "Light Purple (4)" },
         { "LIGHT_YELLOW 26",   "Light Yellow" },
         { "LIGHT_YELLOW_2 27", "Light Yellow (2)" },
         { "WHITE 28",          "White" },
         { NULL, NULL },
      },
      "DEFAULT_ON 00",
   },
   {
      CORE_OPTION_NAME "_vmu3_pixel_off_color",
      "VMU Screen 3 Pixel Off Color",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "DEFAULT_OFF 01", "Default OFF" },
         { "DEFAULT_ON 00",  "Default ON" },
         { "BLACK 02",          "Black" },
         { "BLUE 03",           "Blue" },
         { "LIGHT_BLUE 04",     "Light Blue" },
         { "GREEN 05",          "Green" },
         { "CYAN 06",           "Cyan" },
         { "CYAN_BLUE 07",      "Cyan Blue" },
         { "LIGHT_GREEN 08",    "Light Green" },
         { "CYAN_GREEN 09",     "Cyan Green" },
         { "LIGHT_CYAN 10",     "Light Cyan" },
         { "RED 11",            "Red" },
         { "PURPLE 12",         "Purple" },
         { "LIGHT_PURPLE 13",   "Light Purple" },
         { "YELLOW 14",         "Yellow" },
         { "GRAY 15",           "Gray" },
         { "LIGHT_PURPLE_2 16", "Light Purple (2)" },
         { "LIGHT_GREEN_2 17",  "Light Green (2)" },
         { "LIGHT_GREEN_3 18",  "Light Green (3)" },
         { "LIGHT_CYAN_2 19",   "Light Cyan (2)" },
         { "LIGHT_RED_2 20",    "Light Red (2)" },
         { "MAGENTA 21",        "Magenta" },
         { "LIGHT_PURPLE_3 22",   "Light Purple (3)" },
         { "LIGHT_ORANGE 23",   "Light Orange" },
         { "ORANGE 24",         "Orange" },
         { "LIGHT_PURPLE_4 25", "Light Purple (4)" },
         { "LIGHT_YELLOW 26",   "Light Yellow" },
         { "LIGHT_YELLOW_2 27", "Light Yellow (2)" },
         { "WHITE 28",          "White" },
         { NULL, NULL },
      },
      "DEFAULT_OFF 01",
   },
   {
      CORE_OPTION_NAME "_vmu3_screen_opacity",
      "VMU Screen 3 Opacity",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "10%",  NULL },
         { "20%",  NULL },
         { "30%",  NULL },
         { "40%",  NULL },
         { "50%",  NULL },
         { "60%",  NULL },
         { "70%",  NULL },
         { "80%",  NULL },
         { "90%",  NULL },
         { "100%", NULL },
         { NULL,   NULL },
      },
      "100%",
   },
   {
      CORE_OPTION_NAME "_vmu4_screen_display",
      "VMU Screen 4 Display",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_vmu4_screen_position",
      "VMU Screen 4 Position",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "Upper Left",  NULL },
         { "Upper Right", NULL },
         { "Lower Left",  NULL },
         { "Lower Right", NULL },
         { NULL, NULL },
      },
      "Upper Left",
   },
   {
      CORE_OPTION_NAME "_vmu4_screen_size_mult",
      "VMU Screen 4 Size",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "1x", NULL },
         { "2x", NULL },
         { "3x", NULL },
         { "4x", NULL },
         { "5x", NULL },
         { NULL, NULL },
      },
      "1x",
   },
   {
      CORE_OPTION_NAME "_vmu4_pixel_on_color",
      "VMU Screen 4 Pixel On Color",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "DEFAULT_ON 00",  "Default ON" },
         { "DEFAULT_OFF 01", "Default OFF" },
         { "BLACK 02",          "Black" },
         { "BLUE 03",           "Blue" },
         { "LIGHT_BLUE 04",     "Light Blue" },
         { "GREEN 05",          "Green" },
         { "CYAN 06",           "Cyan" },
         { "CYAN_BLUE 07",      "Cyan Blue" },
         { "LIGHT_GREEN 08",    "Light Green" },
         { "CYAN_GREEN 09",     "Cyan Green" },
         { "LIGHT_CYAN 10",     "Light Cyan" },
         { "RED 11",            "Red" },
         { "PURPLE 12",         "Purple" },
         { "LIGHT_PURPLE 13",   "Light Purple" },
         { "YELLOW 14",         "Yellow" },
         { "GRAY 15",           "Gray" },
         { "LIGHT_PURPLE_2 16", "Light Purple (2)" },
         { "LIGHT_GREEN_2 17",  "Light Green (2)" },
         { "LIGHT_GREEN_3 18",  "Light Green (3)" },
         { "LIGHT_CYAN_2 19",   "Light Cyan (2)" },
         { "LIGHT_RED_2 20",    "Light Red (2)" },
         { "MAGENTA 21",        "Magenta" },
         { "LIGHT_PURPLE_3 22",   "Light Purple (3)" },
         { "LIGHT_ORANGE 23",   "Light Orange" },
         { "ORANGE 24",         "Orange" },
         { "LIGHT_PURPLE_4 25", "Light Purple (4)" },
         { "LIGHT_YELLOW 26",   "Light Yellow" },
         { "LIGHT_YELLOW_2 27", "Light Yellow (2)" },
         { "WHITE 28",          "White" },
         { NULL, NULL },
      },
      "DEFAULT_ON 00",
   },
   {
      CORE_OPTION_NAME "_vmu4_pixel_off_color",
      "VMU Screen 4 Pixel Off Color",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "DEFAULT_OFF 01", "Default OFF" },
         { "DEFAULT_ON 00",  "Default ON" },
         { "BLACK 02",          "Black" },
         { "BLUE 03",           "Blue" },
         { "LIGHT_BLUE 04",     "Light Blue" },
         { "GREEN 05",          "Green" },
         { "CYAN 06",           "Cyan" },
         { "CYAN_BLUE 07",      "Cyan Blue" },
         { "LIGHT_GREEN 08",    "Light Green" },
         { "CYAN_GREEN 09",     "Cyan Green" },
         { "LIGHT_CYAN 10",     "Light Cyan" },
         { "RED 11",            "Red" },
         { "PURPLE 12",         "Purple" },
         { "LIGHT_PURPLE 13",   "Light Purple" },
         { "YELLOW 14",         "Yellow" },
         { "GRAY 15",           "Gray" },
         { "LIGHT_PURPLE_2 16", "Light Purple (2)" },
         { "LIGHT_GREEN_2 17",  "Light Green (2)" },
         { "LIGHT_GREEN_3 18",  "Light Green (3)" },
         { "LIGHT_CYAN_2 19",   "Light Cyan (2)" },
         { "LIGHT_RED_2 20",    "Light Red (2)" },
         { "MAGENTA 21",        "Magenta" },
         { "LIGHT_PURPLE_3 22",   "Light Purple (3)" },
         { "LIGHT_ORANGE 23",   "Light Orange" },
         { "ORANGE 24",         "Orange" },
         { "LIGHT_PURPLE_4 25", "Light Purple (4)" },
         { "LIGHT_YELLOW 26",   "Light Yellow" },
         { "LIGHT_YELLOW_2 27", "Light Yellow (2)" },
         { "WHITE 28",          "White" },
         { NULL, NULL },
      },
      "DEFAULT_OFF 01",
   },
   {
      CORE_OPTION_NAME "_vmu4_screen_opacity",
      "VMU Screen 4 Opacity",
      NULL,
      "",
      NULL,
      "vmu",
      {
         { "10%",  NULL },
         { "20%",  NULL },
         { "30%",  NULL },
         { "40%",  NULL },
         { "50%",  NULL },
         { "60%",  NULL },
         { "70%",  NULL },
         { "80%",  NULL },
         { "90%",  NULL },
         { "100%", NULL },
         { NULL,   NULL },
      },
      "100%",
   },
   {
      CORE_OPTION_NAME "_show_lightgun_settings",
      "Show Light Gun Settings",
      NULL,
      "Enable configuration of light gun crosshair display options. NOTE: Quick Menu must be toggled for this setting to take effect.",
      NULL,
      "input",
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL},
      },
      "disabled"
   },
   {
      CORE_OPTION_NAME "_lightgun1_crosshair",
      "Gun Crosshair 1 Display",
      NULL,
      "",
      NULL,
      "input",
      {
         { "disabled", NULL },
         { "White",    NULL },
         { "Red",      NULL },
         { "Green",    NULL },
         { "Blue",     NULL },
         { NULL,       NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_lightgun2_crosshair",
      "Gun Crosshair 2 Display",
      NULL,
      "",
      NULL,
      "input",
      {
         { "disabled", NULL },
         { "White",    NULL },
         { "Red",      NULL },
         { "Green",    NULL },
         { "Blue",     NULL },
         { NULL,       NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_lightgun3_crosshair",
      "Gun Crosshair 3 Display",
      NULL,
      "",
      NULL,
      "input",
      {
         { "disabled", NULL },
         { "White",    NULL },
         { "Red",      NULL },
         { "Green",    NULL },
         { "Blue",     NULL },
         { NULL,       NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_lightgun4_crosshair",
      "Gun Crosshair 4 Display",
      NULL,
      "",
      NULL,
      "input",
      {
         { "disabled", NULL },
         { "White",    NULL },
         { "Red",      NULL },
         { "Green",    NULL },
         { "Blue",     NULL },
         { NULL,       NULL },
      },
      "disabled",
   },
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};

struct retro_core_options_v2 options_us = {
   option_cats_us,
   option_defs_us
};

/*
 ********************************
 * Language Mapping
 ********************************
*/

#ifndef HAVE_NO_LANGEXTRA
struct retro_core_options_v2 *options_intl[RETRO_LANGUAGE_LAST] = {
   &options_us, /* RETRO_LANGUAGE_ENGLISH */
   &options_ja,      /* RETRO_LANGUAGE_JAPANESE */
   &options_fr,      /* RETRO_LANGUAGE_FRENCH */
   &options_es,      /* RETRO_LANGUAGE_SPANISH */
   &options_de,      /* RETRO_LANGUAGE_GERMAN */
   &options_it,      /* RETRO_LANGUAGE_ITALIAN */
   &options_nl,      /* RETRO_LANGUAGE_DUTCH */
   &options_pt_br,   /* RETRO_LANGUAGE_PORTUGUESE_BRAZIL */
   &options_pt_pt,   /* RETRO_LANGUAGE_PORTUGUESE_PORTUGAL */
   &options_ru,      /* RETRO_LANGUAGE_RUSSIAN */
   &options_ko,      /* RETRO_LANGUAGE_KOREAN */
   &options_cht,     /* RETRO_LANGUAGE_CHINESE_TRADITIONAL */
   &options_chs,     /* RETRO_LANGUAGE_CHINESE_SIMPLIFIED */
   &options_eo,      /* RETRO_LANGUAGE_ESPERANTO */
   &options_pl,      /* RETRO_LANGUAGE_POLISH */
   &options_vn,      /* RETRO_LANGUAGE_VIETNAMESE */
   &options_ar,      /* RETRO_LANGUAGE_ARABIC */
   &options_el,      /* RETRO_LANGUAGE_GREEK */
   &options_tr,      /* RETRO_LANGUAGE_TURKISH */
   &options_sv,      /* RETRO_LANGUAGE_SLOVAK */
   &options_fa,      /* RETRO_LANGUAGE_PERSIAN */
   &options_he,      /* RETRO_LANGUAGE_HEBREW */
   &options_ast,     /* RETRO_LANGUAGE_ASTURIAN */
   &options_fi,      /* RETRO_LANGUAGE_FINNISH */
};
#endif

/*
 ********************************
 * Functions
 ********************************
*/

/* Handles configuration/setting of core options.
 * Should be called as early as possible - ideally inside
 * retro_set_environment(), and no later than retro_load_game()
 * > We place the function body in the header to avoid the
 *   necessity of adding more .c files (i.e. want this to
 *   be as painless as possible for core devs)
 */

static INLINE void libretro_set_core_options(retro_environment_t environ_cb,
      bool *categories_supported)
{
   unsigned version  = 0;
#ifndef HAVE_NO_LANGEXTRA
   unsigned language = 0;
#endif
   /* The definitions options_us has, which are not always the table of
    * that name: libretro.cpp puts an option of its own making among them
    * for a merged romset (merged_set_option()). */
   struct retro_core_option_v2_definition *option_defs_us = options_us.definitions;

   if (!environ_cb || !categories_supported)
      return;

   *categories_supported = false;

   if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
      version = 0;

   if (version >= 2)
   {
#ifndef HAVE_NO_LANGEXTRA
      struct retro_core_options_v2_intl core_options_intl;

      core_options_intl.us    = &options_us;
      core_options_intl.local = NULL;

      if (environ_cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &language) &&
          (language < RETRO_LANGUAGE_LAST) && (language != RETRO_LANGUAGE_ENGLISH))
         core_options_intl.local = options_intl[language];

      *categories_supported = environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL,
            &core_options_intl);
#else
      *categories_supported = environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2,
            &options_us);
#endif
   }
   else
   {
      size_t i, j;
      size_t option_index              = 0;
      size_t num_options               = 0;
      struct retro_core_option_definition
            *option_v1_defs_us         = NULL;
#ifndef HAVE_NO_LANGEXTRA
      size_t num_options_intl          = 0;
      struct retro_core_option_v2_definition
            *option_defs_intl          = NULL;
      struct retro_core_option_definition
            *option_v1_defs_intl       = NULL;
      struct retro_core_options_intl
            core_options_v1_intl;
#endif
      struct retro_variable *variables = NULL;
      char **values_buf                = NULL;

      /* Determine total number of options */
      while (true)
      {
         if (option_defs_us[num_options].key)
            num_options++;
         else
            break;
      }

      if (version >= 1)
      {
         /* Allocate US array */
         option_v1_defs_us = (struct retro_core_option_definition *)
               calloc(num_options + 1, sizeof(struct retro_core_option_definition));

         /* Copy parameters from option_defs_us array */
         for (i = 0; i < num_options; i++)
         {
            struct retro_core_option_v2_definition *option_def_us = &option_defs_us[i];
            struct retro_core_option_value *option_values         = option_def_us->values;
            struct retro_core_option_definition *option_v1_def_us = &option_v1_defs_us[i];
            struct retro_core_option_value *option_v1_values      = option_v1_def_us->values;

            option_v1_def_us->key           = option_def_us->key;
            option_v1_def_us->desc          = option_def_us->desc;
            option_v1_def_us->info          = option_def_us->info;
            option_v1_def_us->default_value = option_def_us->default_value;

            /* Values must be copied individually... */
            while (option_values->value)
            {
               option_v1_values->value = option_values->value;
               option_v1_values->label = option_values->label;

               option_values++;
               option_v1_values++;
            }
         }

#ifndef HAVE_NO_LANGEXTRA
         if (environ_cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &language) &&
             (language < RETRO_LANGUAGE_LAST) && (language != RETRO_LANGUAGE_ENGLISH) &&
             options_intl[language])
            option_defs_intl = options_intl[language]->definitions;

         if (option_defs_intl)
         {
            /* Determine number of intl options */
            while (true)
            {
               if (option_defs_intl[num_options_intl].key)
                  num_options_intl++;
               else
                  break;
            }

            /* Allocate intl array */
            option_v1_defs_intl = (struct retro_core_option_definition *)
                  calloc(num_options_intl + 1, sizeof(struct retro_core_option_definition));

            /* Copy parameters from option_defs_intl array */
            for (i = 0; i < num_options_intl; i++)
            {
               struct retro_core_option_v2_definition *option_def_intl = &option_defs_intl[i];
               struct retro_core_option_value *option_values           = option_def_intl->values;
               struct retro_core_option_definition *option_v1_def_intl = &option_v1_defs_intl[i];
               struct retro_core_option_value *option_v1_values        = option_v1_def_intl->values;

               option_v1_def_intl->key           = option_def_intl->key;
               option_v1_def_intl->desc          = option_def_intl->desc;
               option_v1_def_intl->info          = option_def_intl->info;
               option_v1_def_intl->default_value = option_def_intl->default_value;

               /* Values must be copied individually... */
               while (option_values->value)
               {
                  option_v1_values->value = option_values->value;
                  option_v1_values->label = option_values->label;

                  option_values++;
                  option_v1_values++;
               }
            }
         }

         core_options_v1_intl.us    = option_v1_defs_us;
         core_options_v1_intl.local = option_v1_defs_intl;

         environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL, &core_options_v1_intl);
#else
         environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, option_v1_defs_us);
#endif
      }
      else
      {
         /* Allocate arrays */
         variables  = (struct retro_variable *)calloc(num_options + 1,
               sizeof(struct retro_variable));
         values_buf = (char **)calloc(num_options, sizeof(char *));

         if (!variables || !values_buf)
            goto error;

         /* Copy parameters from option_defs_us array */
         for (i = 0; i < num_options; i++)
         {
            const char *key                        = option_defs_us[i].key;
            const char *desc                       = option_defs_us[i].desc;
            const char *default_value              = option_defs_us[i].default_value;
            struct retro_core_option_value *values = option_defs_us[i].values;
            size_t buf_len                         = 3;
            size_t default_index                   = 0;

            values_buf[i] = NULL;

            if (desc)
            {
               size_t num_values = 0;

               /* Determine number of values */
               while (true)
               {
                  if (values[num_values].value)
                  {
                     /* Check if this is the default value */
                     if (default_value)
                        if (strcmp(values[num_values].value, default_value) == 0)
                           default_index = num_values;

                     buf_len += strlen(values[num_values].value);
                     num_values++;
                  }
                  else
                     break;
               }

               /* Build values string */
               if (num_values > 0)
               {
                  buf_len += num_values - 1;
                  buf_len += strlen(desc);

                  values_buf[i] = (char *)calloc(buf_len, sizeof(char));
                  if (!values_buf[i])
                     goto error;

                  strcpy(values_buf[i], desc);
                  strcat(values_buf[i], "; ");

                  /* Default value goes first */
                  strcat(values_buf[i], values[default_index].value);

                  /* Add remaining values */
                  for (j = 0; j < num_values; j++)
                  {
                     if (j != default_index)
                     {
                        strcat(values_buf[i], "|");
                        strcat(values_buf[i], values[j].value);
                     }
                  }
               }
            }

            variables[option_index].key   = key;
            variables[option_index].value = values_buf[i];
            option_index++;
         }

         /* Set variables */
         environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
      }

error:
      /* Clean up */

      if (option_v1_defs_us)
      {
         free(option_v1_defs_us);
         option_v1_defs_us = NULL;
      }

#ifndef HAVE_NO_LANGEXTRA
      if (option_v1_defs_intl)
      {
         free(option_v1_defs_intl);
         option_v1_defs_intl = NULL;
      }
#endif

      if (values_buf)
      {
         for (i = 0; i < num_options; i++)
         {
            if (values_buf[i])
            {
               free(values_buf[i]);
               values_buf[i] = NULL;
            }
         }

         free(values_buf);
         values_buf = NULL;
      }

      if (variables)
      {
         free(variables);
         variables = NULL;
      }
   }
}

#ifdef __cplusplus
}
#endif

#endif
