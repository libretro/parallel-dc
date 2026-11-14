/*
	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.

    Some PLFO and FEG code from Highly_Theoretical lib by Neill Corlett
    (https://github.com/kode54/Highly_Theoretical)
 */

#include <math.h>
#include <string.h>

/* (first: its INLINE, which forces, before libretro-common's, which
 * only suggests) */
#include "sgc_if.h"
#include <compat/intrinsics.h>

#include "dsp.h"
#include "aica_mem.h"
#include "aica_if.h"
#include "hw/naomi/midiffb.h"
#include "oslib/audiostream.h"
#undef FAR

/* #define CLIP_WARN */
#define key_printf(...) DEBUG_LOG(AICA, __VA_ARGS__)
#define aeg_printf(...) DEBUG_LOG(AICA, __VA_ARGS__)
#define feg_printf(...) DEBUG_LOG(AICA, __VA_ARGS__)
#define step_printf(...) DEBUG_LOG(AICA, __VA_ARGS__)

#ifdef CLIP_WARN
#define clip_verify(x) verify(x)
#else
#define clip_verify(x)
#endif

/* Sound generation, mixin, and channel regs emulation */
/* x.15 */
static s32 volume_lut[16];
/* What each output gets of a signal sent at a level and a pan (EFSDL and
 * EFPAN), x.15: [0] left, [1] right, by level * 32 + pan. The two are steps
 * of 3 dB, and for the side the pan turns down they are one attenuation,
 * their sum. */
static s32 volpan_lut[2][16 * 32];
/* 255 -> mute */
/* Converts Send levels to TL-compatible values (DISDL, etc) */
static const u32 SendLevel[16] =
{
	255, 14 << 3, 13 << 3, 12 << 3, 11 << 3, 10 << 3, 9 << 3, 8 << 3,
	7 << 3, 6 << 3, 5 << 3, 4 << 3, 3 << 3, 2 << 3, 1 << 3, 0 << 3
};
/* xx.15 format, by attenuation in the envelope's units: it has 10 bits, 64
 * of its steps to 6 dB, and TL and the send levels are a quarter as fine, 16
 * to 6 dB. The last entries, from 961, are 0. */
static s32 tl_lut[1024];

/* in ms :) */
static const double AEG_Attack_Time[64] =
{
	-1,-1,8100.0,6900.0,6000.0,4800.0,4000.0,3400.0,3000.0,2400.0,2000.0,1700.0,1500.0,
	1200.0,1000.0,860.0,760.0,600.0,500.0,430.0,380.0,300.0,250.0,220.0,190.0,150.0,130.0,110.0,95.0,
	76.0,63.0,55.0,47.0,38.0,31.0,27.0,24.0,19.0,15.0,13.0,12.0,9.4,7.9,6.8,6.0,4.7,3.8,3.4,3.0,2.4,
	2.0,1.8,1.6,1.3,1.1,0.93,0.85,0.65,0.53,0.44,0.40,0.35,0.0,0.0
};
static const double AEG_DSR_Time[64] =
{
	-1,-1,118200.0,101300.0,88600.0,70900.0,59100.0,50700.0,44300.0,35500.0,29600.0,25300.0,22200.0,17700.0,
	14800.0,12700.0,11100.0,8900.0,7400.0,6300.0,5500.0,4400.0,3700.0,3200.0,2800.0,2200.0,1800.0,1600.0,1400.0,1100.0,
	920.0,790.0,690.0,550.0,460.0,390.0,340.0,270.0,230.0,200.0,170.0,140.0,110.0,98.0,85.0,68.0,57.0,49.0,43.0,34.0,
	28.0,25.0,22.0,18.0,14.0,12.0,11.0,8.5,7.1,6.1,5.4,4.3,3.6,3.1
};
static const float PLFOS_Scale[8] = { 0.f, 3.61f, 7.22f, 14.44f, 28.88f, 57.75f, 115.5f, 231.f };
/* What the pitch LFO multiplies the pitch by, with PLFO_BITS bits below 1.
 * (There were 10, which is steps of 1.7 cents, and the values were cut down
 * to them: every one below the middle of the wave was up to a step flat.) */
#define PLFO_BITS 20
static int PLFO_Scales[8][256];

/* A channel's place between two samples, and what it moves on by for every
 * sample put out, have 18 bits below a whole sample, as the chip's phase
 * has: the pitch is (1024 + FNS) << (OCT + 8) of them, OCT from -8 to 7,
 * and no bit of FNS is lost in any octave. (There were 10, and the pitch
 * was shifted down into them: a note was flat by up to 1.7 cents an octave
 * down, 5 two down, 12 three down.) */
#define STEP_BITS 18
#define STEP_ONE (1u << STEP_BITS)

#define EG_STEP_BITS (16)
#define AEG_ATTACK_SHIFT 16

/* Steps per sample */
static u32 AEG_ATT_SPS[64];
static u32 AEG_DSR_SPS[64];
static u32 FEG_SPS[64];

static const char* stream_names[]=
{
	"0: 16-bit PCM (two's complement format)",
	"1: 8-bit PCM (two's complement format)",
	"2: 4-bit ADPCM (Yamaha format)",
	"3: 4-bit ADPCM long stream"
};

/* x.8 format */
static const s32 adpcm_qs[8] =
{
	0x0e6, 0x0e6, 0x0e6, 0x0e6, 0x133, 0x199, 0x200, 0x266,
};
/* x.3 format */
static const s32 adpcm_scale[16] =
{
	1,3,5,7,9,11,13,15,
	-1,-3,-5,-7,-9,-11,-13,-15,
};

/* What each value of the Q register does to the low-pass filter's damping,
 * in 4096ths of it: more at the top, which takes the edge off the corner,
 * and less and less below, which leaves a peak there. */
static const s32 qtable[32] = {
	 2048,  1536,  1024,   512,
	    0,  -256,  -512,  -768,
	-1024, -1280, -1536, -1792,
	-2048, -2176, -2304, -2432,
	-2560, -2688, -2816, -2944,
	-3072, -3136, -3200, -3264,
	-3328, -3392, -3456, -3520,
	-3584, -3648, -3712, -3776
};

/* Remove the fractional part by chopping.. */
#define FPChop(a, bits) ((a) >> bits)

#define FPs FPChop
/* Fixed point mul w/ rounding :) */
#define FPMul(a, b, bits) (FPs((a) * (b), bits))

/* A 16-bit signal into the two outputs, which are summed as 20 bits (see
 * AICA_Sample()). One multiplication for each side. There used to be two,
 * one after the other, for the side the pan turns down: the level's, cut
 * back to 16 bits, and then the pan's on what was left, which came out up
 * to 2 below what the two together make of the signal. */
#define VOLPAN(value,vol,pan,outl,outr) \
{\
	const s32 t_value=(value);\
	const u32 t_lvl=((vol)<<5)|(pan);\
	(outl)+=FPMul(t_value,volpan_lut[0][t_lvl],11);\
	(outr)+=FPMul(t_value,volpan_lut[1][t_lvl],11);\
}

DSP_OUT_VOL_REG* dsp_out_vol;

#pragma pack(push, 1)
/* All regs are 16b , aligned to 32b (upper bits 0?) */
typedef struct ChannelCommonData ChannelCommonData;
struct ChannelCommonData
{
	/* +00 [0] */
	/* SA is half at reg 0 and the rest at reg 1 */
	u32 SA_hi:7;
	u32 PCMS:2;
	u32 LPCTL:1;
	u32 SSCTL:1;
	u32 res_1:3;
	u32 KYONB:1;
	u32 KYONEX:1;

	u32 pad_2:16;

	/* +04 [1] */
	/* SA (defined above) */
	u32 SA_low:16;

	u32 pad_3:16;

	/* +08 [2] */
	u32 LSA:16;

	u32 pad_4:16;

	/* +0C [3] */
	u32 LEA:16;

	u32 pad_5:16;

	/* +10 [4] */
	u32 AR:5;
	u32 res_2:1;
	u32 D1R:5;
	u32 D2R:5;

	u32 pad_7:16;

	/* +14 [5] */
	u32 RR:5;
	u32 DL:5;
	u32 KRS:4;
	u32 LPSLNK:1;
	u32 res_3:1;

	u32 pad_8:16;

	/* +18[6] */
	u32 FNS:10;
	u32 rez_8_1:1;
	u32 OCT:4;
	u32 rez_8_2:1;

	u32 pad_9:16;

	/* +1C	RE	LFOF[4:0]	PLFOWS	PLFOS[2:0]	ALFOWS	ALFOS[2:0] */
	u32 ALFOS:3;
	u32 ALFOWS:2;

	u32 PLFOS:3;
	u32 PLFOWS:2;

	u32 LFOF:5;
	u32 LFORE:1;

	u32 pad_10:16;

	/* +20	--	IMXL[3:0]	ISEL[3:0] */
	u32 ISEL:4;
	u32 IMXL:4;
	u32 rez_20_0:8;

	u32 pad_11:16;

	/* +24	--	DISDL[3:0]	--	DIPAN[4:0] */
	u32 DIPAN:5;
	u32 rez_24_0:3;

	u32 DISDL:4;
	u32 rez_24_1:4;

	u32 pad_12:16;


	/* +28	TL[7:0]	--	Q[4:0] */
	u32 Q:5;
	u32 LPOFF:1;		/* confirmed but not documented: 0: LPF enabled, 1: LPF disabled */
	u32 VOFF:1;			/* unconfirmed: 0: attenuation enabled, 1: attenuation disabled (TL, AEG, ALFO) */
	u32 rez_28_0:1;

	u32 TL:8;

	u32 pad_13:16;

	/* +2C	--	FLV0[12:0] */
	u32 FLV0:13;
	u32 rez_2C_0:3;

	u32 pad_14:16;

	/* +30	--	FLV1[12:0] */
	u32 FLV1:13;
	u32 rez_30_0:3;

	u32 pad_15:16;

	/* +34	--	FLV2[12:0] */
	u32 FLV2:13;
	u32 rez_34_0:3;

	u32 pad_16:16;

	/* +38	--	FLV3[12:0] */
	u32 FLV3:13;
	u32 rez_38_0:3;

	u32 pad_17:16;

	/* +3C	--	FLV4[12:0] */
	u32 FLV4:13;
	u32 rez_3C_0:3;

	u32 pad_18:16;

	/* +40	--	FAR[4:0]	--	FD1R[4:0] */
	u32 FD1R:5;
	u32 rez_40_0:3;
	u32 FAR:5;
	u32 rez_40_1:3;

	u32 pad_19:16;

	/* +44	--	FD2R[4:0]	--	FRR[4:0] */
	u32 FRR:5;
	u32 rez_44_0:3;
	u32 FD2R:5;
	u32 rez_44_1:3;

	u32 pad_20:16;
};
#pragma pack(pop)


typedef enum _EG_state
{
	EG_Attack = 0,
	EG_Decay1 = 1,
	EG_Decay2 = 2,
	EG_Release = 3
} _EG_state;

/*
	KEY_OFF->KEY_ON : Resets everything, and starts playback (EG: A)
	KEY_ON->KEY_ON  : nothing
	KEY_ON->KEY_OFF : Switches to RELEASE state (does not disable channel)

*/

typedef struct ChannelEx ChannelEx;
typedef void (*channel_fn)(ChannelEx* ch);

static channel_fn STREAM_STEP_LUT[5][2][2];
static channel_fn STREAM_INITAL_STEP_LUT[5];
/* The same for a channel that could read past the end of sound memory, by
 * format: see ch_update_stream_step(). */
static channel_fn STREAM_STEP_WRAP_LUT[4];
static channel_fn STREAM_INITAL_STEP_WRAP_LUT[4];
static channel_fn AEG_STEP_LUT[4];
static channel_fn FEG_STEP_LUT[4];
static channel_fn ALFOWS_CALC[4];
static channel_fn PLFOWS_CALC[4];

struct ChannelEx
{
	ChannelCommonData* ccd;

	u8* SA;
	u32 CA;
	u32 step;			/* below a sample: STEP_BITS bits */
	u32 update_rate;	/* the pitch, from OCT and FNS */
	u32 step_inc;		/* and with the pitch LFO: see ch_update_step() */

	SampleType s0,s1;

	struct
	{
		u32 LSA;
		u32 LEA;

		u8 looped;
	} loop;

	struct
	{
		/* used in adpcm decoding */
		s32 last_quant;
		/* Saved quantization and previous sample used in PCMS mode 2 (yamaha ADPCM) */
		s32 loopstart_quant;
		SampleType loopstart_prev_sample;
		bool in_loop;
	} adpcm;

	u32 noise_state;/* for Noise generator */

	struct
	{
		u32 DLAtt;
		u32 DRAtt;
		u32 DSPAtt;
		SampleType* DSPOut;
	} VolMix;

	channel_fn StepAEG;
	channel_fn StepFEG;
	channel_fn StepStream;
	channel_fn StepStreamInitial;

	struct
	{
		s32 val;
		_EG_state state;

		u32 AttackRate;
		u32 Decay1Rate;
		u32 Decay2Value;
		u32 Decay2Rate;
		u32 ReleaseRate;
	} AEG;

	struct
	{
		u32 value;
		_EG_state state;

		SampleType prev1;
		SampleType prev2;
		/* What the filter's last sample dropped below its lowest bit, to
		 * go into the next one: see ch_low_pass_filter() */
		s32 fract;
		/* The filter's coefficients, and the cutoff they were worked out
		 * for: no cutoff is as high as the one this starts at (~0u, made
		 * so by sgc_Init() before anything else) */
		u32 coef_fv;
		s64 a0, b1, b2;
		s32 q;
		u32 AttackRate;
		u32 Decay1Rate;
		u32 Decay2Rate;
		u32 ReleaseRate;
		bool active;
	} FEG;

	struct
	{
		u32 counter;
		u32 start_value;
		u8 state;
		u8 alfo;
		u8 alfo_shft;
		u32 plfo_step;
		int *plfo_scale;
		channel_fn alfo_calc;
		channel_fn plfo_calc;
	} lfo;

	bool enabled;	/* set to false to 'freeze' the channel */
	int ChannelNumber;
};

static ChannelEx Chans[64];
/* One bit for every channel that is enabled, bit n for channel n: the
 * channels step_all() has anything to do for. ch_enable(), ch_disable()
 * and ch_refresh_active() are the only places it changes. */
static u64 chans_active;

static void ch_reg_write(ChannelEx *ch, u32 offset, int size);
static void ch_update_sa(ChannelEx *ch);
static void ch_update_loop(ChannelEx *ch);
static void ch_update_pitch(ChannelEx *ch);
static void ch_update_step(ChannelEx *ch);
static void ch_update_aeg(ChannelEx *ch);
static void ch_update_feg(ChannelEx *ch);
static void ch_update_lfo(ChannelEx *ch, bool from_state);
static void ch_update_dsp_mix(ChannelEx *ch);
static void ch_update_atts(ChannelEx *ch);
static void ch_update_stream_step(ChannelEx *ch);

static INLINE s32 aeg_get(const ChannelEx *ch) { return ch->AEG.val>>EG_STEP_BITS; }
static void aeg_set(ChannelEx *ch, u32 aegb) { ch->AEG.val=aegb<<EG_STEP_BITS; }
static INLINE u32 feg_get(const ChannelEx *ch) { return ch->FEG.value >> EG_STEP_BITS; }
static void feg_set(ChannelEx *ch, u32 fegb) { ch->FEG.value = fegb << EG_STEP_BITS; }

static void ch_adpcm_reset(ChannelEx *ch)
{
	ch->adpcm.last_quant=127;
	ch->adpcm.loopstart_quant = 0;
	ch->adpcm.loopstart_prev_sample = 0;
	ch->adpcm.in_loop = false;

	ch->s0=0;
}

static INLINE void ch_lfo_step(ChannelEx *ch)
{
	ch->lfo.counter--;
	if (ch->lfo.counter==0)
	{
		ch->lfo.state++;
		ch->lfo.counter=ch->lfo.start_value;
		ch->lfo.alfo_calc(ch);
		ch->lfo.plfo_calc(ch);
	}
}

static void ch_lfo_reset(ChannelEx *ch)
{
	ch->lfo.state=0;
	ch->lfo.counter=ch->lfo.start_value;
	ch->lfo.alfo_calc(ch);
	ch->lfo.plfo_calc(ch);
}

static void ch_lfo_set_start_value(ChannelEx *ch, u32 nv)
{
	ch->lfo.start_value=nv;
	ch->lfo.counter=ch->lfo.start_value;
}

static void ch_set_aeg_state(ChannelEx *ch, _EG_state newstate)
{
	ch->StepAEG=AEG_STEP_LUT[newstate];
	ch->AEG.state=newstate;
	if (newstate==EG_Release)
		ch->ccd->KYONB=0;
}

static void ch_set_feg_state(ChannelEx *ch, _EG_state newstate)
{
	ch->StepFEG=FEG_STEP_LUT[newstate];
	ch->FEG.state=newstate;
	if (newstate == EG_Attack)
	{
		feg_set(ch, ch->ccd->FLV0);
		ch->FEG.prev1 = 0;
		ch->FEG.prev2 = 0;
		ch->FEG.fract = 0;
	}
}

static void ch_disable(ChannelEx *ch)
{
	ch->enabled=false;
	chans_active &= ~((u64)1 << ch->ChannelNumber);
	ch_set_aeg_state(ch, EG_Release);
	aeg_set(ch, 0x3FF);
	/* The position reads 0 once a channel has stopped, whether it
	 * came to its end or its envelope ran out: some games wait for
	 * that. */
	ch->CA = 0;
}

static void ch_enable(ChannelEx *ch)
{
	ch->enabled=true;
	chans_active |= (u64)1 << ch->ChannelNumber;
}

/* After the enabled flags were set some other way: a state was loaded */
static void ch_refresh_active(void)
{
	int i;

	chans_active = 0;
	for (i = 0; i < 64; i++)
		if (Chans[i].enabled)
			chans_active |= (u64)1 << i;
}

static void ch_init(ChannelEx *ch, int cn, u8* ccd_raw)
{
	u32 i;

	ch->ccd=(ChannelCommonData*)&ccd_raw[cn*0x80];
	ch->ChannelNumber = cn;
	for (i = 0; i < 0x80; i += 2)
		ch_reg_write(ch, i, 2);
	ch_disable(ch);
}

/* The sample between the last one read and the next, by how far the
 * channel has moved on from the one towards the other, as 20 bits.
 *
 * The two products are added and then scaled, once, and the four bits
 * below a 16-bit sample that the scaling used to throw away are kept:
 * the filter and the DSP both work on 20. Scaling each product on its
 * own first, as this did, lost up to two units of the 16 - between two
 * equal samples it could come out one below either - before a single
 * zero was shifted in for the other four. Exactly on a sample, which is
 * every sample of a sound played at the rate it was made for, the two
 * ways give the same.
 *
 * The top 14 bits of the place between the two are used, which with
 * samples of 16 stays well inside 32. */
static INLINE SampleType ch_interpolate_sample(const ChannelEx *ch)
{
	const s32 fp = ch->step >> (STEP_BITS - 14);

	return (ch->s0 * (16384 - fp) + ch->s1 * fp) >> 10;
}

/* The channel's low-pass filter, on a 20-bit sample: two poles, with
 * resonance.
 *
 *    y = -a0 x + (2 - f - a0) y1 - (1 - f) y2
 *
 * The cutoff register is a 4-bit exponent and a 9-bit mantissa, sixteen
 * octaves from wide open. Out of it come the corner frequency, as the
 * damping term f, and its square, a0, which is how much of the input
 * gets in. The Q register scales f: less damping, more of a peak at the
 * corner, from none to some 22 dB. The level at DC is exactly that of
 * the input, whatever the settings, with the sign turned over.
 *
 * This is the filter of upstream flycast, coefficient for coefficient.
 * What it replaces took the same a0 but a damping that did not follow
 * the cutoff down. Wide open or at the highest Q the two agree on where
 * the corner is; anywhere else the old one was overdamped, the more so
 * the lower the cutoff - 64 Hz for a setting whose corner is at 400 -
 * and its Q register made a peak only near the top of either range.
 * Here a Q value makes the same peak at every cutoff.
 *
 * Thirty bits of fraction are worked in. The ones the output cannot hold
 * are kept in FEG.fract and go into the next sample, so that nothing
 * the feedback loop drops builds up in it. At the lowest exponent
 * nothing of the input gets in at all and the filter holds its level;
 * there the fraction is dropped, or the level would creep. */
static INLINE SampleType ch_low_pass_filter(ChannelEx *ch, SampleType sample)
{
	const u32 fv = feg_get(ch) & 0x1FFF;
	s64 mac;

	/* The coefficients follow from the cutoff and Q alone, and the
	 * cutoff only moves when the envelope takes it to another of its
	 * 8192 values: they are worked out then and kept. ch_update_feg()
	 * forgets them when the Q register may have changed. */
	if (fv != ch->FEG.coef_fv)
	{
		const u32 exp = fv >> 9;
		const u32 mant = (fv & 0x1FF) | 0x200;
		u64 a0 = ((u64)mant << 30) >> ((15 - exp) * 2);
		s64 f;
		a0 *= (mant - 1) / 8;
		a0 >>= 17;
		f = ((s64)mant << exp) << 5;
		f += (s64)ch->FEG.q * f / 4096;
		ch->FEG.coef_fv = fv;
		ch->FEG.a0 = (s64)a0;
		ch->FEG.b1 = ((s64)1 << 31) - (f + (s64)a0);
		ch->FEG.b2 = ((s64)1 << 30) - f;
	}
	if (fv < 0x200)
		ch->FEG.fract = 0;

	mac = -ch->FEG.a0 * sample + ch->FEG.b1 * ch->FEG.prev1 - ch->FEG.b2 * ch->FEG.prev2 - ch->FEG.fract;
	sample = (SampleType)(mac >> 30);
	/* (a multiply: a negative sample shifted left is undefined) */
	ch->FEG.fract = (s32)((s64)sample * ((s64)1 << 30) - mac);
	ch->FEG.prev2 = ch->FEG.prev1;
	if (sample < -512 * 1024)
		sample = -512 * 1024;
	else if (sample > 512 * 1024 - 1)
		sample = 512 * 1024 - 1;
	ch->FEG.prev1 = sample;
	return sample;
}

/* An enabled channel's sample into the outputs and its DSP input */
static INLINE void ch_step(ChannelEx *ch, SampleType *mixl, SampleType *mixr)
{
	/* 20 bits from here to the outputs: the filter needs the four
	 * extra, and the DSP takes 20-bit samples. */
	SampleType sample = ch_interpolate_sample(ch);
	SampleType oLeft, oRight, oDsp;
	u32 ofsatt, max_att, dl, dr, ds;
	s32 *logtable;

	if (ch->FEG.active)
		sample = ch_low_pass_filter(ch, sample);

	/* Volume & Mixer processing */
	/* All attenuations are added together then applied and mixed :) */

	/* offset is up to 1023 */
	/* logtable handles up to 1023, anything >=961 is mute */

	/* In the envelope's units, all 10 bits of it. The envelope
	 * used to be brought down to the 8 of the others, and the level
	 * went by steps of 0.375 dB where the chip's are 0.094.
	 * The amplitude LFO is in those units too: see ch_update_lfo(). */
	if (ch->ccd->VOFF == 1)
	{
		ofsatt = 0;
	}
	else
	{
		ofsatt = ch->lfo.alfo + aeg_get(ch);
		/* make sure it never gets more 1023 -- it can happen with some alfo/aeg combinations */
		if (ofsatt > 1023)
			ofsatt = 1023;
	}
	max_att = 1023 - ofsatt;

	logtable = ofsatt + tl_lut;

	dl = ch->VolMix.DLAtt < max_att ? ch->VolMix.DLAtt : max_att;
	dr = ch->VolMix.DRAtt < max_att ? ch->VolMix.DRAtt : max_att;
	ds = ch->VolMix.DSPAtt < max_att ? ch->VolMix.DSPAtt : max_att;

	/* 20 bits to the outputs too, as to the DSP: they are added up
	 * as that, and what is below 16 bits goes once, from the sum,
	 * and not from each channel before it is added. */
	oLeft = (SampleType)FPMul((s64)sample, (s64)logtable[dl], 15);	/* 20 bits */
	oRight = (SampleType)FPMul((s64)sample, (s64)logtable[dr], 15);	/* 20 bits */
	oDsp = (SampleType)FPMul((s64)sample, (s64)logtable[ds], 15);	/* 20 bits */

	clip_verify((oLeft << 12) >> 12 == oLeft);
	clip_verify((oRight << 12) >> 12 == oRight);
	clip_verify((oDsp << 12) >> 12 == oDsp);
	clip_verify((s64)sample*oLeft>=0);
	clip_verify((s64)sample*oRight>=0);
	clip_verify((s64)sample*oDsp>=0);

	ch->StepAEG(ch);
	/* (a channel its envelope has just stopped stays at position 0) */
	if (ch->enabled)
	{
		ch->StepFEG(ch);
		ch->StepStream(ch);
		ch_lfo_step(ch);
	}

	*ch->VolMix.DSPOut+=oDsp;
	*mixl+=oLeft;
	*mixr+=oRight;
}

/* Only the channels that are on. A channel that is off adds nothing
 * to anything, and going to all 64 to find that out - a load, and an
 * add of zero into its DSP input - was most of what this cost with a
 * handful of them playing. A channel may switch itself off as it is
 * stepped; that changes the set, not this pass over a copy of it.
 *
 * From one that is on straight to the next, lowest first as before:
 * not past every number in between. Games do not use the low ones
 * first - Virtua Fighter 4 has one or two channels on with numbers up
 * to 35, Dead or Alive 2 a dozen spread over forty. */
static INLINE void step_all(SampleType *mixl, SampleType *mixr)
{
	u64 on = chans_active;
	for (; on != 0; on &= on - 1)
		ch_step(&Chans[compat_ctz_u64(on)], mixl, mixr);
}

static void ch_key_on(ChannelEx *ch)
{
	if (ch->AEG.state != EG_Release)
		return;

	/* if it was off then turn it on ! */
	ch_enable(ch);

	/* reset AEG */
	ch_set_aeg_state(ch, EG_Attack);
	aeg_set(ch, 0x280);	/* start value taken from HT */

	/* reset FEG */
	ch_set_feg_state(ch, EG_Attack);

	/* Reset sampling state */
	ch->CA=0;
	ch->step=0;

	ch->loop.looped=false;

	ch_adpcm_reset(ch);

	ch->StepStreamInitial(ch);
}

static void ch_key_off(ChannelEx *ch)
{
	if (ch->AEG.state ==EG_Release)
		return;

	ch_set_aeg_state(ch, EG_Release);
	ch_set_feg_state(ch, EG_Release);
}

/* PCMS,SSCTL,LPCTL,LPSLNK, and SA,LSA,LEA for where the samples end */
static void ch_update_stream_step(ChannelEx *ch)
{
	s32 fmt=ch->ccd->PCMS;
	u32 last = 0, end;

	if (ch->ccd->SSCTL)
		fmt=4;

	ch->StepStream=STREAM_STEP_LUT[fmt][ch->ccd->LPCTL][ch->ccd->LPSLNK];
	ch->StepStreamInitial=STREAM_INITAL_STEP_LUT[fmt];

	if (fmt == 4)
		return;	/* noise: nothing is read */
	/* The last sample the channel can get to with its registers as they
	 * are: the one before the loop end, where the position goes back to
	 * the loop start or the channel stops, or the loop start itself if
	 * that is further on. An ADPCM stream may get three past the loop
	 * end, see stream_step_any(). The sample after the one playing is read
	 * as well, and is one of these too.
	 *
	 * If that sample ends beyond sound memory the channel gets the
	 * functions that bring every address back round to the start of
	 * memory, as the chip does. No real program has such a channel, and
	 * all the others are spared a check on each sample they read: this
	 * is done again whenever one of those registers is written. */
	if (ch->loop.LEA != 0)
	{
		last = ch->loop.LEA - 1;
		if (fmt == 3)
			last |= 3;
	}
	if (last < ch->loop.LSA)
		last = ch->loop.LSA;
	/* in bytes: a sample is two, one, or half of one */
	end = fmt == 0 ? last * 2 + 2 : fmt == 1 ? last + 1 : last / 2 + 1;
	if ((u32)(ch->SA - aica_ram.data) + end > ARAM_SIZE)
	{
		ch->StepStream = STREAM_STEP_WRAP_LUT[fmt];
		ch->StepStreamInitial = STREAM_INITAL_STEP_WRAP_LUT[fmt];
	}
}

/* SA,PCMS */
static void ch_update_sa(ChannelEx *ch)
{
	u32 addr = (ch->ccd->SA_hi<<16) | ch->ccd->SA_low;
	if (ch->ccd->PCMS==0)
		addr&=~1; /* 0: 16 bit */
	/* The register has 23 bits and sound memory is 2 MB of that on a
	 * Dreamcast: an address above it is the same memory again, and
	 * where the host has no mirrors of it, was a place past its end. */
	addr &= ARAM_MASK;

	ch->SA=&aica_ram.data[addr];
}

/* LSA,LEA */
static void ch_update_loop(ChannelEx *ch)
{
	ch->loop.LSA=ch->ccd->LSA;
	ch->loop.LEA=ch->ccd->LEA;
}

static s32 ch_eg_base_rate(const ChannelEx *ch)
{
	/* The key scaling adds twice KRS plus the octave, which is
	 * signed, as the chip's manual says; when that sum is below zero
	 * the chip adds nothing for it (tested on hardware), it does not
	 * slow the envelope down. */
	s32 effrate = 0;
	if (ch->ccd->KRS < 0xF)
	{
		const s32 ks = ((s32)ch->ccd->KRS + (s32)(ch->ccd->OCT ^ 8) - 8) * 2;
		effrate += (ch->ccd->FNS >> 9) & 1;
		effrate += ks > 0 ? ks : 0;
	}

	return effrate;
}

static u32 ch_eg_eff_rate(s32 base_rate, u32 rate)
{
	s32 rv = base_rate + rate * 2;

	clip(rv, 0, 0x3f);

	return rv;
}

/* D2R,D1R,AR,DL,RR,KRS, [OCT,FNS] for now */
static void ch_update_aeg(ChannelEx *ch)
{
	s32 base_rate = ch_eg_base_rate(ch);
	ch->AEG.AttackRate = AEG_ATT_SPS[ch_eg_eff_rate(base_rate, ch->ccd->AR)];
	ch->AEG.Decay1Rate = AEG_DSR_SPS[ch_eg_eff_rate(base_rate, ch->ccd->D1R)];
	ch->AEG.Decay2Value = ch->ccd->DL<<5;
	ch->AEG.Decay2Rate = AEG_DSR_SPS[ch_eg_eff_rate(base_rate, ch->ccd->D2R)];
	ch->AEG.ReleaseRate = AEG_DSR_SPS[ch_eg_eff_rate(base_rate, ch->ccd->RR)];
}

/* OCT,FNS */
static void ch_update_pitch(ChannelEx *ch)
{
	u32 oct=ch->ccd->OCT;

	/* (OCT is -8 to 7 in four bits: with the top one turned over, 0 to 15) */
	ch->update_rate = (1024 | ch->ccd->FNS) << (oct ^ 8);
	ch_update_step(ch);
}

/* What the place in the sound moves on by for every sample: the pitch,
 * times what the pitch LFO makes of it at this point of its wave. It
 * is worked out here, when one of the two changes, and each sample only
 * adds it: they used to be multiplied for every sample of every
 * channel. At most 2047 << 15 times 1.143, which 32 bits hold. */
static void ch_update_step(ChannelEx *ch)
{
	ch->step_inc = (u32)(((u64)ch->update_rate * ch->lfo.plfo_step) >> PLFO_BITS);
}

/* LFORE,LFOF,PLFOWS,PLFOS,ALFOWS,ALFOS */
/* @from_state: the channel has just been read out of a save state,
 * with where its LFO was in it. What follows from the registers is
 * worked out again; the count and the step it had got to are the
 * state's and are left. (They were started again with the rest, and a
 * loaded state's vibrato and tremolo set off from the top of the wave:
 * upstream has had this since 2021, as "derivedState".) */
static void ch_update_lfo(ChannelEx *ch, bool from_state)
{
	{
		int N=ch->ccd->LFOF;
		int S = N >> 2;
		int M = (~N) & 3;
		int G = 128>>S;
		int L = (G-1)<<2;
		int O = L + G * (M+1);
		/* (with both waves random the frequency does not count) */
		if (ch->ccd->ALFOWS == 3 && ch->ccd->PLFOWS == 3)
			O = 1;
		if (from_state)
			ch->lfo.start_value = O;
		else
			ch_lfo_set_start_value(ch, O);
	}

	/* The wave, 0 to 255, comes down by 7 - ALFOS and is added to the
	 * attenuation in the envelope's units, 64 to 6 dB: at 7 that is up
	 * to 255 of them, 24 dB, and half as many for each setting below,
	 * which is what the chip's table of depths says (0.4, 0.8, 1.5, 3,
	 * 6, 12 and 24 dB). At 0 there is none. It used to come down by
	 * 8 - ALFOS and be added in units four times as big, which was
	 * twice as deep at every setting: 47.6 dB at 7. */
	ch->lfo.alfo_shft = ch->ccd->ALFOS ? 7 - ch->ccd->ALFOS : 8;

	ch->lfo.alfo_calc=ALFOWS_CALC[ch->ccd->ALFOWS];
	ch->lfo.plfo_calc=PLFOWS_CALC[ch->ccd->PLFOWS];
	ch->lfo.plfo_scale = PLFO_Scales[ch->ccd->PLFOS];

	if (ch->ccd->LFORE && !from_state)
	{
		ch_lfo_reset(ch);
	}
	else
	{
		ch->lfo.alfo_calc(ch);
		ch->lfo.plfo_calc(ch);
	}
}

/* ISEL */
static void ch_update_dsp_mix(ChannelEx *ch)
{
	ch->VolMix.DSPOut = &dsp.MIXS[ch->ccd->ISEL];
}

/* TL,DISDL,DIPAN,IMXL, and Mono */
/* (in the envelope's units, a quarter of TL's: see ch_step()) */
static void ch_update_atts(ChannelEx *ch)
{
	u32 total_level = ch->ccd->VOFF ? 0 : (ch->ccd->TL << 2);
	u32 attFull = total_level + (SendLevel[ch->ccd->DISDL] << 2);
	/* (in mono the pan is not used: the channel is as loud on both sides) */
	const u32 pan = CommonData->Mono ? 0 : ch->ccd->DIPAN;
	u32 attPan=attFull+(SendLevel[(~pan)&0xF] << 2);

	/* 0x1* -> R decreases */
	if (pan&0x10)
	{
		ch->VolMix.DLAtt=attFull;
		ch->VolMix.DRAtt=attPan;
	}
	else /* 0x0* -> L decreases */
	{
		ch->VolMix.DLAtt=attPan;
		ch->VolMix.DRAtt=attFull;
	}

	ch->VolMix.DSPAtt = total_level + (SendLevel[ch->ccd->IMXL] << 2);
}

/* Q,FLV0,FLV1,FLV2,FLV3,FLV4,FAR,FD1R,FD2R,FRR, LPOFF */
static void ch_update_feg(ChannelEx *ch)
{
	s32 base_rate;

	ch->FEG.active = ch->ccd->LPOFF == 0
			&& (ch->ccd->FLV0 < 0x1ff7 || ch->ccd->FLV1 < 0x1ff7
					|| ch->ccd->FLV2 < 0x1ff7 || ch->ccd->FLV3 < 0x1ff7
					|| ch->ccd->FLV4 < 0x1ff7);
	if (!ch->FEG.active)
		return;
	feg_printf("FEG active channel %d Q %d FLV: %05x %05x %05x %05x %05x AR %02x FD1R %02x FD2R %02x FRR %02x\n",
			ch->ChannelNumber, ch->ccd->Q,
			ch->ccd->FLV0, ch->ccd->FLV1, ch->ccd->FLV2, ch->ccd->FLV3, ch->ccd->FLV4,
			ch->ccd->FAR, ch->ccd->FD1R, ch->ccd->FD2R, ch->ccd->FRR);
	ch->FEG.q = qtable[ch->ccd->Q];
	ch->FEG.coef_fv = ~0u;
	base_rate = ch_eg_base_rate(ch);
	ch->FEG.AttackRate = FEG_SPS[ch_eg_eff_rate(base_rate, ch->ccd->FAR)];
	ch->FEG.Decay1Rate = FEG_SPS[ch_eg_eff_rate(base_rate, ch->ccd->FD1R)];
	ch->FEG.Decay2Rate = FEG_SPS[ch_eg_eff_rate(base_rate, ch->ccd->FD2R)];
	ch->FEG.ReleaseRate = FEG_SPS[ch_eg_eff_rate(base_rate, ch->ccd->FRR)];
}

/* WHEE :D! */
static void ch_reg_write(ChannelEx *ch, u32 offset, int size)
{
	switch(offset)
	{
	case 0x00:
	case 0x01:
		ch_update_sa(ch);
		ch_update_stream_step(ch);
		if ((offset == 0x01 || size == 2) && ch->ccd->KYONEX)
		{
			int i;
			ch->ccd->KYONEX=0;
			for (i = 0; i < 64; i++)
			{
				if (Chans[i].ccd->KYONB)
					ch_key_on(&Chans[i]);
				else
					ch_key_off(&Chans[i]);
			}
		}
		break;

	case 0x04:
	case 0x05:
		ch_update_sa(ch);
		ch_update_stream_step(ch);
		break;

	case 0x08:/* LSA */
	case 0x09:/* LSA */
	case 0x0C:/* LEA */
	case 0x0D:/* LEA */
		ch_update_loop(ch);
		ch_update_stream_step(ch);
		break;

	case 0x10:/* D1R,AR */
	case 0x11:/* D2R,D1R */
		ch_update_aeg(ch);
		break;

	/* The rates of both envelopes go by the key scaling, the octave
	 * and FNS (ch_eg_base_rate()). The filter's were worked out again
	 * only when one of its own registers was written: a note bent or
	 * transposed went on with its filter stepping at the rate of the
	 * note before. (And a state loaded had them worked out from the
	 * registers as they are, so the game left alone and the game
	 * loaded from a state went on with another filter.) */
	case 0x14:/* RR,DL */
	case 0x15:/* DL,KRS,LPSLINK */
		ch_update_stream_step(ch);
		ch_update_aeg(ch);
		ch_update_feg(ch);
		break;

	case 0x18:/* FNS */
	case 0x19:/* FNS,OCT */
		ch_update_pitch(ch);
		ch_update_aeg(ch);
		ch_update_feg(ch);
		break;

	case 0x1C:/* ALFOS,ALFOWS,PLFOS */
	case 0x1D:/* PLFOWS,LFOF,LFORE */
		ch_update_lfo(ch, false);
		break;

	case 0x20:/* ISEL,IMXL */
	/* case 0x21: nothing here ! */
		ch_update_dsp_mix(ch);
		ch_update_atts(ch);
		break;

	case 0x24:/* DIPAN */
	case 0x25:/* DISDL */
		ch_update_atts(ch);
		break;

	case 0x28:/* Q, LPOFF, VOFF */
	case 0x29:/* TL */
		if (size == 2 || offset == 0x28)
			ch_update_feg(ch);
		/* (either byte: VOFF takes TL out of the levels) */
		ch_update_atts(ch);
		break;

	case 0x2C: /* FLV0 */
	case 0x2D: /* FLV0 */
	case 0x30: /* FLV1 */
	case 0x31: /* FLV1 */
	case 0x34: /* FLV2 */
	case 0x35: /* FLV2 */
	case 0x38: /* FLV3 */
	case 0x39: /* FLV3 */
	case 0x3C: /* FLV4 */
	case 0x3D: /* FLV4 */
	case 0x40: /* FD1R */
	case 0x41: /* FAR */
	case 0x44: /* FRR */
	case 0x45: /* FD2R */
		ch_update_feg(ch);
		break;

	}
}

static INLINE SampleType decode_adpcm(u32 sample, s32 prev, s32 *quant)
{
	s32 sign=1-2*(sample/8);

	u32 data=sample&7;

	/*(1 - 2 * L4) * (L3 + L2/2 +L1/4 + 1/8) * quantized width (Dn) + decode value (Xn - 1) */
	SampleType rv = (*quant * adpcm_scale[data]) >> 3;
	if (rv > 0x7FFF)
		rv = 0x7FFF;
	rv = sign * rv + prev;

	*quant = (*quant * adpcm_qs[data])>>8;

	clip(*quant,127,24576);
	clip16(rv);
	return rv;
}

/* The functions below with a format, a loop mode or a state among their
 * arguments are always inlined, and called only with constants for those:
 * every combination the tables at the end hold is a function of its own,
 * made by the macros there, with no test of any of them left in it. */

/* Where a channel's sample data is, @offset bytes on from its start
 * address. With @wrap, for a channel that ch_update_stream_step() found
 * could get past the end of sound memory, the address comes back round to
 * the start of memory; it stays even for 16-bit samples, whose start
 * address is. Every other channel reads straight from its pointer. */
static INLINE u8* sample_data(ChannelEx* ch, size_t offset, bool wrap)
{
	if (wrap)
		return &aica_ram.data[(ch->SA - aica_ram.data + offset) & ARAM_MASK];
	return ch->SA + offset;
}

/* @PCMS: the format, -1 for noise. @last: the last sample of this step,
 * whose next one is read as well. */
static INLINE void step_decode_sample(ChannelEx* ch, u32 CA, s32 PCMS, bool last, bool wrap)
{
	u32 next_addr, lea;
	SampleType s0,s1;

	if (!last && PCMS<2)
		return ;

	next_addr = CA + 1;
	/* (A long stream goes back to the loop start at the multiple of four
	 * at or after the loop end, see stream_step_any(): the sample after the
	 * ones before that is the next one, not the loop start's.) */
	lea = PCMS == 3 ? (ch->loop.LEA + 3) & ~3u : ch->loop.LEA;
	/* (With the loop start after the loop end the channel plays on past
	 * the end, to the loop start, and stops there: see stream_step_any().) */
	if (next_addr >= lea && lea > ch->loop.LSA)
		next_addr = ch->loop.LSA;

	switch(PCMS)
	{
	case -1:
		/* The ANSI C generator, as Highly Theoretical's noise: each of the
		 * two samples a new number. */
		ch->noise_state = ch->noise_state * 0x41c64e6d + 0x3039;
		s0=ch->noise_state;
		s0>>=16;

		ch->noise_state = ch->noise_state * 0x41c64e6d + 0x3039;
		s1=ch->noise_state;
		s1>>=16;
		break;

	case 0:
		s0 = *(s16*)sample_data(ch, (size_t)CA * 2, wrap);
		s1 = *(s16*)sample_data(ch, (size_t)next_addr * 2, wrap);
		break;

	case 1:
		/* (a multiply: a negative sample shifted left is undefined) */
		s0 = *(s8*)sample_data(ch, CA, wrap) * 256;
		s1 = *(s8*)sample_data(ch, next_addr, wrap) * 256;
		break;

	default: /* 2, 3 */
		{
			u8 ad1 = *sample_data(ch, CA>>1, wrap);
			u8 ad2 = *sample_data(ch, next_addr >> 1, wrap);
			s32 q;

			ad1 >>= (CA & 1) * 4;
			ad2 >>= (next_addr & 1) * 4;

			ad1&=0xF;
			ad2&=0xF;

			q=ch->adpcm.last_quant;
			/* The decoder's state at the loop start is kept the first time
			 * there in a long stream too, which does not go back to it: a
			 * game may change the channel to plain ADPCM as it plays, and the
			 * loop then goes back to what was kept (NBA 2K). */
			if (CA == ch->loop.LSA)
			{
				if (!ch->adpcm.in_loop)
				{
					ch->adpcm.in_loop = true;
					ch->adpcm.loopstart_quant = q;
					ch->adpcm.loopstart_prev_sample = ch->s0;
				}
				else if (PCMS == 2)
				{
					q = ch->adpcm.loopstart_quant;
					ch->s0 = ch->adpcm.loopstart_prev_sample;
				}
			}
			s0=decode_adpcm(ad1,ch->s0,&q);
			ch->adpcm.last_quant=q;
			if (last)
			{
				SampleType prev = s0;
				if (PCMS == 2 && next_addr == ch->loop.LSA && ch->adpcm.in_loop)
				{
					q = ch->adpcm.loopstart_quant;
					prev = ch->adpcm.loopstart_prev_sample;
				}
				s1 = decode_adpcm(ad2, prev, &q);
			}
			else
				s1=0;
		}
		break;
	}

	ch->s0=s0;
	ch->s1=s1;
}

/* What the stream steps below are made of. */
static INLINE void stream_step_any(ChannelEx* ch, s32 PCMS, bool wrap, u32 LPCTL, u32 LPSLNK)
{
	const u32 sp=ch->step + ch->step_inc;
	/* whole samples to move on by */
	u32 ip=sp >> STEP_BITS;

	ch->step=sp & (STEP_ONE - 1);

	while(ip>0)
	{
		u32 CA, ca_t, lea;

		ip--;

		CA=ch->CA + 1;

		ca_t=CA;
		if (PCMS==3)
			ca_t&=~3;	/* in adpcm "stream" mode, LEA and LSA are supposed to be 4-sample aligned */
					/* but some games don't respect this rule */

		if (LPSLNK)
		{
			if ((ch->AEG.state==EG_Attack) && (CA>=ch->loop.LSA))
			{
				step_printf("[%d]LPSLNK : Switching to EG_Decay1 %X\n", ch->ChannelNumber, aeg_get(ch));
				ch_set_aeg_state(ch, EG_Decay1);
			}
		}

		/* (the loop end as ca_t is held against it) */
		lea = PCMS == 3 ? (ch->loop.LEA + 3) & ~3u : ch->loop.LEA;
		if (ca_t >= ch->loop.LEA && ch->loop.LSA > lea)
		{
			/* With the loop start after the loop end the chip does not
			 * stop at the loop end but plays on to the loop start; there
			 * the position goes back to 0 and the channel stops, looping
			 * or not (tested on hardware). */
			if (CA >= ch->loop.LSA)
			{
				ch->loop.looped=1;
				CA = 0;
				ch_disable(ch);
			}
		}
		else if (ca_t>=ch->loop.LEA)
		{
			ch->loop.looped=1;
			if (LPCTL == 0)
			{
				CA = 0;
				ch_disable(ch);
			}
			else
				CA=ch->loop.LSA;
		}

		ch->CA=CA;

		/* keep adpcm up to date */
		if (ip==0)
			step_decode_sample(ch, CA, PCMS, true, wrap);
		else
			step_decode_sample(ch, CA, PCMS, false, wrap);
	}
}

/* For the channel that could read past the end of sound memory, the loop
 * mode is looked up as it goes rather than built in: how fast that one is
 * does not matter. */
static INLINE void stream_step_wrap(ChannelEx* ch, s32 PCMS)
{
	stream_step_any(ch, PCMS, true, ch->ccd->LPCTL, ch->ccd->LPSLNK);
}

/* The random wave of the LFOs: a value that looks like noise for each step
 * of the LFO, the same for the same step. With both LFOs on it, the LFO
 * steps every sample, whatever its frequency: see ch_update_lfo(). (The old
 * mix of shifts of the step was far from noise; Jet Set Radio's skating
 * sound is made with this wave.) */
static INLINE u32 lfo_random(u8 state)
{
	return (state * 0x41c64e6du + 0x3039u) & 0xFF;
}

/* The LFO's wave at its step: 0 saw, 1 square, 2 triangle, 3 random */
static INLINE u32 lfo_wave(const ChannelEx* ch, s32 ws)
{
	switch(ws)
	{
	case 0:
		return ch->lfo.state;
	case 1:
		return ch->lfo.state&0x80?255:0;
	case 2:
		return ((ch->lfo.state&0x7f)^(ch->lfo.state&0x80 ? 0x7F:0)) << 1;
	default: /* 8 bits, see lfo_random() */
		return lfo_random(ch->lfo.state);
	}
}

static INLINE void calc_alfo(ChannelEx* ch, s32 ALFOWS)
{
	ch->lfo.alfo=lfo_wave(ch, ALFOWS)>>ch->lfo.alfo_shft;
}

static INLINE void calc_plfo(ChannelEx* ch, s32 PLFOWS)
{
	ch->lfo.plfo_step = ch->lfo.plfo_scale[(u8)lfo_wave(ch, PLFOWS)];
	ch_update_step(ch);
}

static INLINE void aeg_step(ChannelEx* ch, u32 state)
{
	switch(state)
	{
	case EG_Attack:
		if (ch->AEG.AttackRate != 0)
		{
			ch->AEG.val -= (((u64)ch->AEG.val << AEG_ATTACK_SHIFT) / ch->AEG.AttackRate) + 1;
			if (aeg_get(ch)<=0)
			{
				if (!ch->ccd->LPSLNK)
				{
					aeg_printf("[%d]AEG_step : Switching to EG_Decay1\n", ch->ChannelNumber);
					ch_set_aeg_state(ch, EG_Decay1);
				}
				aeg_set(ch, 0);
			}
		}
		break;
	case EG_Decay1:
		ch->AEG.val+=ch->AEG.Decay1Rate;
		if (((u32)aeg_get(ch))>=ch->AEG.Decay2Value)
		{
			aeg_printf("[%d]AEG_step : Switching to EG_Decay2\n", ch->ChannelNumber);
			ch_set_aeg_state(ch, EG_Decay2);
		}
		break;
	case EG_Decay2:
		ch->AEG.val+=ch->AEG.Decay2Rate;
		if (aeg_get(ch)>=0x3FF)
		{
			aeg_printf("[%d]AEG_step : Switching to EG_Release\n", ch->ChannelNumber);
			aeg_set(ch, 0x3FF);
			ch_set_aeg_state(ch, EG_Release);
		}
		break;
	default: /* EG_Release: only on key_off */
		ch->AEG.val+=ch->AEG.ReleaseRate;

		if (aeg_get(ch)>=0x3FF)
		{
			aeg_printf("[%d]AEG_step : EG_Release End @ %x\n", ch->ChannelNumber, aeg_get(ch));
			aeg_set(ch, 0x3FF); /* TODO: mnn, should we do anything about it running wild ? */
			ch_disable(ch); /* TODO: Is this ok here? It's a speed optimisation (since the channel is muted) */
		}
		break;
	}
}

static INLINE void feg_step(ChannelEx* ch, u32 state)
{
	u32 delta;
	u32 target;

	if (!ch->FEG.active)
		return;
	switch(state)
	{
	case EG_Attack:
		delta = ch->FEG.AttackRate;
		target = ch->ccd->FLV1;
		break;
	case EG_Decay1:
		delta = ch->FEG.Decay1Rate;
		target = ch->ccd->FLV2;
		break;
	case EG_Decay2:
		delta = ch->FEG.Decay2Rate;
		target = ch->ccd->FLV3;
		break;
	default: /* EG_Release */
		delta = ch->FEG.ReleaseRate;
		target = ch->ccd->FLV4;
		break;
	}
	target <<= EG_STEP_BITS;
	if (ch->FEG.value < target)
	{
		u32 maxd = target - ch->FEG.value;
		if (delta > maxd)
			delta = maxd;
		ch->FEG.value += delta;
	}
	else if (ch->FEG.value > target)
	{
		u32 maxd = ch->FEG.value - target;
		if (delta > maxd)
			delta = maxd;
		ch->FEG.value -= delta;
	}
	else if (ch->FEG.state < EG_Decay2)
		ch_set_feg_state(ch, (_EG_state)((int)ch->FEG.state + 1));
}

/* The functions the tables point to. A format of -1 is the noise
 * generator: N1 in their names. */
#define SGC_STREAM_STEP(name, PCMS, LPCTL, LPSLNK) \
	static void name(ChannelEx* ch) { stream_step_any(ch, PCMS, false, LPCTL, LPSLNK); }
#define SGC_STREAM_STEPS(n, PCMS) \
	SGC_STREAM_STEP(stream_step_##n##_00, PCMS, 0, 0) \
	SGC_STREAM_STEP(stream_step_##n##_01, PCMS, 0, 1) \
	SGC_STREAM_STEP(stream_step_##n##_10, PCMS, 1, 0) \
	SGC_STREAM_STEP(stream_step_##n##_11, PCMS, 1, 1) \
	static void stream_initial_##n(ChannelEx* ch) { step_decode_sample(ch, 0, PCMS, true, false); }
#define SGC_STREAM_WRAP(n) \
	static void stream_step_wrap_##n(ChannelEx* ch) { stream_step_wrap(ch, n); } \
	static void stream_initial_wrap_##n(ChannelEx* ch) { step_decode_sample(ch, 0, n, true, true); }
#define SGC_PER_4(n) \
	static void aeg_step_##n(ChannelEx* ch) { aeg_step(ch, n); } \
	static void feg_step_##n(ChannelEx* ch) { feg_step(ch, n); } \
	static void calc_alfo_##n(ChannelEx* ch) { calc_alfo(ch, n); } \
	static void calc_plfo_##n(ChannelEx* ch) { calc_plfo(ch, n); }

SGC_STREAM_STEPS(0, 0)
SGC_STREAM_STEPS(1, 1)
SGC_STREAM_STEPS(2, 2)
SGC_STREAM_STEPS(3, 3)
SGC_STREAM_STEPS(N1, -1)
SGC_STREAM_WRAP(0)
SGC_STREAM_WRAP(1)
SGC_STREAM_WRAP(2)
SGC_STREAM_WRAP(3)
SGC_PER_4(0)
SGC_PER_4(1)
SGC_PER_4(2)
SGC_PER_4(3)

#define SGC_FILL_STREAM(i, n) \
	STREAM_STEP_LUT[i][0][0] = stream_step_##n##_00; \
	STREAM_STEP_LUT[i][0][1] = stream_step_##n##_01; \
	STREAM_STEP_LUT[i][1][0] = stream_step_##n##_10; \
	STREAM_STEP_LUT[i][1][1] = stream_step_##n##_11; \
	STREAM_INITAL_STEP_LUT[i] = stream_initial_##n
#define SGC_FILL_4(n) \
	STREAM_STEP_WRAP_LUT[n] = stream_step_wrap_##n; \
	STREAM_INITAL_STEP_WRAP_LUT[n] = stream_initial_wrap_##n; \
	AEG_STEP_LUT[n] = aeg_step_##n; \
	FEG_STEP_LUT[n] = feg_step_##n; \
	ALFOWS_CALC[n] = calc_alfo_##n; \
	PLFOWS_CALC[n] = calc_plfo_##n

static void staticinitialise(void)
{
	SGC_FILL_STREAM(0, 0);
	SGC_FILL_STREAM(1, 1);
	SGC_FILL_STREAM(2, 2);
	SGC_FILL_STREAM(3, 3);
	SGC_FILL_STREAM(4, N1);
	SGC_FILL_4(0);
	SGC_FILL_4(1);
	SGC_FILL_4(2);
	SGC_FILL_4(3);
}


/* To the nearest whole number, a half up: round_positive() for the positive values
 * these tables have (C89 has no lround) */
static long round_positive(double x)
{
	return (long)floor(x + 0.5);
}

static u32 CalcEgSteps(float t)
{
	const double eg_allsteps = 1024 * (1 << EG_STEP_BITS) - 1;

	if (t < 0)
		return 0;
	if (t == 0)
		return (u32)eg_allsteps;

	{
		/* 44.1*ms = samples */
		double scnt = 44.1 * t;
		double steps = eg_allsteps / scnt;
		return (u32)round_positive(steps);
	}
}
static u32 CalcAttackEgSteps(float t)
{
	if (t<0)
		return 0;
	if (t==0)
		return 1 << AEG_ATTACK_SHIFT;

	{
		/* 44.1*ms = samples */
		double scnt=44.1*t;
		double factor = (1.0 / (1.0 - 1.0 / pow(0x280, 1.0 / scnt))) * (1 << AEG_ATTACK_SHIFT);

		return (u32)round_positive(factor);
	}
}
void sgc_Init(void)
{
	int i, vol, pan, s;

	staticinitialise();
	/* (what is waiting in the MIDI input goes with the chip's reset) */
	midiffb_reset();

	for (i=0;i<16;i++)
	{
		volume_lut[i]=(s32)((1<<15)/pow(2.0,(15-i)/2.0));
		if (i==0)
			volume_lut[i]=0;
	}

	for (i=0;i<1024;i++)
		tl_lut[i]=(s32)((1<<15)/pow(2.0,i/64.0));

	for (vol=0;vol<16;vol++)
	{
		for (pan=0;pan<32;pan++)
		{
			/* the side the pan leaves alone, and the one it turns down: */
			/* all the way at 15 */
			s32 full=volume_lut[vol];
			s32 less=0;
			if (vol!=0 && (pan&0xF)!=0xF)
				less=(s32)((1<<15)/pow(2.0,((15-vol)+(pan&0xF))/2.0));
			/* 0x1* -> R decreases */
			volpan_lut[0][vol*32+pan]=(pan&0x10)?full:less;
			volpan_lut[1][vol*32+pan]=(pan&0x10)?less:full;
		}
	}

	for (i=0;i<64;i++)
	{
		AEG_ATT_SPS[i] = CalcAttackEgSteps(AEG_Attack_Time[i]);
		AEG_DSR_SPS[i]=CalcEgSteps(AEG_DSR_Time[i]);
		FEG_SPS[i] = CalcEgSteps(AEG_DSR_Time[i]);
	}
	for (i=0;i<64;i++)
	{
		/* (no cutoff is as high as this: the filter's coefficients are
		 * worked out the first time it runs) */
		Chans[i].FEG.coef_fv = ~0u;
		ch_init(&Chans[i], i,aica_reg);
	}
	dsp_out_vol=(DSP_OUT_VOL_REG*)&aica_reg[0x2000];

	for (s = 0; s < 8; s++)
	{
		float limit = PLFOS_Scale[s];
		for (i = -128; i < 128; i++)
			PLFO_Scales[s][i + 128] = (int)round_positive((1 << PLFO_BITS) * pow(2.0, (double)limit * i / 128.0 / 1200.0));
	}

	dsp_init();
}

void sgc_Term(void)
{
   dsp_term();
}

void WriteChannelReg(u32 channel, u32 reg, int size)
{
	ch_reg_write(&Chans[channel], reg, size);
}

void ReadCommonReg(u32 reg,bool byte)
{
	switch(reg)
	{
	case 0x2808:
	case 0x2809:
		{
			/* The MIDI input has what a driving game's force feedback board
			 * sends (midiffb.cpp). Reading the low byte takes one out. */
			const bool pop = !byte || reg == 0x2808;
			const int midi_in = midiffb_enabled ? midiffb_in_byte(pop) : -1;
			if (midi_in >= 0)
			{
				if (pop)
					CommonData->MIBUF = midi_in;
				CommonData->MIEMP = 0;
				CommonData->MIFUL = 1;
			}
			else
			{
				CommonData->MIEMP = 1;
				CommonData->MIFUL = 0;
			}
		}
		CommonData->MOEMP = 1;
      CommonData->MIOVF = 0;
		CommonData->MOFUL = 0;
		break;
	case 0x2810: /* EG, SGC, LP */
	case 0x2811:
		{
			u32 chan=CommonData->MSLC;
			s32 aeg;

			CommonData->LP=Chans[chan].loop.looped;
			/* (With AFSEL set the program is asking for the filter's
			 * envelope here. It gets this one, the level's, all the same.) */
			aeg = aeg_get(&Chans[chan]);
			if (aeg > 0x3BF)
				CommonData->EG = 0x1FFF;
			else
				CommonData->EG = aeg; /* AEG is only 10 bits, FEG is 13 bits */
			CommonData->SGC=Chans[chan].AEG.state;

			if (! (byte && reg==0x2810))
				Chans[chan].loop.looped=0;
		}
		break;
	case 0x2814: /* CA */
	case 0x2815: /* CA */
		{
			u32 chan=CommonData->MSLC;
			const ChannelEx *ch = &Chans[chan];
			CommonData->CA = ch->CA;
			/* The chip is not seen to report a position within a few
			 * samples of the loop end (on hardware it is hard to read
			 * one past LEA - 60), and the NBA and NFL 2K games miss the
			 * end of a sound when they read LEA - 2 or more: the channel
			 * is then never let go of, and the game freezes later. */
			if (ch->loop.LEA > ch->loop.LSA && ch->loop.LEA > 3
					&& (u32)CommonData->CA > ch->loop.LEA - 3)
				CommonData->CA = ch->loop.LEA - 3;
		}
		break;
	}
}

void WriteCommonReg8(u32 reg,u32 data)
{
	const u32 mono = CommonData->Mono;
	mem_arr_write(aica_reg, reg, data, 1);
	if (reg==0x2804 || reg==0x2805)
	{
		dsp.RBL=(8192<<CommonData->RBL)-1;
		dsp.RBP = (CommonData->RBP * 2048) & ARAM_MASK;
		dsp.dyndirty=true;
	}
	else if (reg == 0x2801 && CommonData->Mono != mono)
	{
		/* every channel's levels have its pan in them */
		int i;
		for (i = 0; i < 64; i++)
			ch_update_atts(&Chans[i]);
	}
	else if (reg == 0x280c && midiffb_enabled)	/* MOBUF: the MIDI output */
		midiffb_receive(data);
}

s16 cdda_sector[CDDA_SIZE]={0};
u32 cdda_index=CDDA_SIZE<<1;
/* The sector being mixed: lent from the disc image, or cdda_sector. */
static const s16* cdda_src = cdda_sector;

void libCore_CDDA_Detach(void)
{
	if (cdda_src != cdda_sector)
	{
		memcpy(cdda_sector, cdda_src, sizeof(cdda_sector));
		cdda_src = cdda_sector;
	}
}

void AICA_Sample(void)
{
	SampleType mixl,mixr;
	s32 EXTS0L, EXTS0R, val;
	u32 panmask, mvol;
	int i;

	mixl = 0;
	mixr = 0;
	memset(dsp.MIXS,0,sizeof(dsp.MIXS));

	step_all(&mixl,&mixr);

	/* OK , generated all Channels  , now DSP/ect + final mix ;p */
	/* CDDA EXTS input */

	/* (Two samples are taken, at an even place. A save state could put this
	 * on the last, odd one, and the second was then past the sector.) */
	if (cdda_index >= CDDA_SIZE - 1)
	{
		cdda_index=0;
		cdda_src = libCore_CDDA_Sector(cdda_sector);
	}
	EXTS0L=cdda_src[cdda_index];
	EXTS0R=cdda_src[cdda_index+1];
	cdda_index+=2;

	/* Final MIX .. */
	/* Add CDDA / DSP effect(s) */

	/* In mono the pans are not used, the channels' (ch_update_atts()) and
	 * these: everything is as loud on both sides as a sound in the middle.
	 * (The two sides used to be added, which made a sound in the middle
	 * twice as loud, and one panned to a side half as loud as that.) */
	panmask = CommonData->Mono ? 0 : 0x1F;

	/* CDDA */
	VOLPAN(EXTS0L,dsp_out_vol[16].EFSDL,dsp_out_vol[16].EFPAN & panmask,mixl,mixr);
	VOLPAN(EXTS0R,dsp_out_vol[17].EFSDL,dsp_out_vol[17].EFPAN & panmask,mixl,mixr);

	DSPData->EXTS[0] = EXTS0L;
	DSPData->EXTS[1] = EXTS0R;
	dsp_step();

	/* A DSP output sent at level 0 is multiplied by 0: left out */
	for (i=0;i<16;i++)
		if (dsp_out_vol[i].EFSDL)
			VOLPAN(*(s16*)&DSPData->EFREG[i], dsp_out_vol[i].EFSDL, dsp_out_vol[i].EFPAN & panmask, mixl, mixr);

	/* MVOL ! */
	/* we want to make sure mix* is *At least* 23 bits wide here, so 64 bit mul ! */
	/* (the sum is of 20-bit samples, and comes down to the 16 of the output */
	/* here, after the volume) */
	mvol=CommonData->MVOL;
	val=volume_lut[mvol];
	mixl=(s32)FPMul((s64)mixl,val,19);
	mixr=(s32)FPMul((s64)mixr,val,19);


	if (CommonData->DAC18B)
	{
		/* If 18 bit output , make it 16b :p */
		mixl=FPs(mixl,2);
		mixr=FPs(mixr,2);
	}

	/* Sample is ready ! clip/saturate and store :} */

#ifdef CLIP_WARN
	if (((s16)mixl) != mixl)
		printf("Clipped mixl %d\n",mixl);
	if (((s16)mixr) != mixr)
		printf("Clipped mixr %d\n",mixr);
#endif

	clip16(mixl);
	clip16(mixr);

	WriteSample(mixr,mixl);
}

bool channel_serialize(void **data, unsigned int *total_size)
{
	int i = 0 ;
	int addr = 0 ;

	for ( i = 0 ; i < 64 ; i++)
	{
		u32 step;

		addr = (int)(Chans[i].SA - aica_ram.data);
		LIBRETRO_S(addr);

		LIBRETRO_S(Chans[i].CA) ;
		/* The 10 bits below a sample that states have always had here, in
		 * the same four bytes; the 8 bits below those are in
		 * channel_serialize_v21(). */
		step = Chans[i].step >> (STEP_BITS - 10);
		LIBRETRO_S(step) ;
		LIBRETRO_S(Chans[i].s0) ;
		LIBRETRO_S(Chans[i].s1) ;
		LIBRETRO_S(Chans[i].loop.looped) ;
		LIBRETRO_S(Chans[i].adpcm.last_quant) ;
		LIBRETRO_S(Chans[i].adpcm.loopstart_quant);
		LIBRETRO_S(Chans[i].adpcm.loopstart_prev_sample);
		LIBRETRO_S(Chans[i].adpcm.in_loop);
		LIBRETRO_S(Chans[i].noise_state) ;

		LIBRETRO_S(Chans[i].AEG.val) ;
		LIBRETRO_S(Chans[i].AEG.state) ;
		LIBRETRO_S(Chans[i].FEG.value);
		LIBRETRO_S(Chans[i].FEG.state);
		LIBRETRO_S(Chans[i].FEG.prev1);
		LIBRETRO_S(Chans[i].FEG.prev2);
		LIBRETRO_S(Chans[i].FEG.fract);

		LIBRETRO_S(Chans[i].lfo.counter) ;
		LIBRETRO_S(Chans[i].lfo.state) ;
		LIBRETRO_S(Chans[i].enabled) ;
	}

	return true;
}

bool channel_unserialize(void **data, unsigned int *total_size, serialize_version_enum ver)
{
	int i = 0 ;
	int addr = 0 ;
	u32 dum;

	for ( i = 0 ; i < 64 ; i++)
	{
		u32 step = 0;

		LIBRETRO_US(addr);
		/* From the channel's registers, as when they are written, and not
		 * the state's own figure: nothing says that one is inside sound
		 * memory, or even for 16-bit samples, and states made before the
		 * start address was kept to the size of memory hold some that are
		 * not. */
		ch_update_sa(&Chans[i]);

		LIBRETRO_US(Chans[i].CA) ;
		/* (Only what is below a sample: there is never anything above it
		 * between two samples, and a state that had something there made
		 * the channel run on by that many samples in one go.) */
		LIBRETRO_US(step) ;
		Chans[i].step = (step & 1023) << (STEP_BITS - 10);
		if (ver < V8)
			LIBRETRO_US(dum); /* Chans[i].update_rate */
		ch_update_pitch(&Chans[i]);
		LIBRETRO_US(Chans[i].s0) ;
		LIBRETRO_US(Chans[i].s1) ;
		LIBRETRO_US(Chans[i].loop.looped);
		if (ver < V8)
		{
			LIBRETRO_US(dum); /* Chans[i].loop.LSA */
			LIBRETRO_US(dum); /* Chans[i].loop.LEA */
		}
		ch_update_loop(&Chans[i]);
		LIBRETRO_US(Chans[i].adpcm.last_quant) ;
		if (ver >= V8)
		{
			LIBRETRO_US(Chans[i].adpcm.loopstart_quant);
			LIBRETRO_US(Chans[i].adpcm.loopstart_prev_sample);
			LIBRETRO_US(Chans[i].adpcm.in_loop);
		}
		else
		{
			Chans[i].adpcm.in_loop = true;
			Chans[i].adpcm.loopstart_quant = 0;
			Chans[i].adpcm.loopstart_prev_sample = 0;
		}
		LIBRETRO_US(Chans[i].noise_state) ;
		if (ver < V8)
		{
			LIBRETRO_US(dum); /* Chans[i].VolMix.DLAtt */
			LIBRETRO_US(dum); /* Chans[i].VolMix.DRAtt */
			LIBRETRO_US(dum); /* Chans[i].VolMix.DSPAtt */
		}
		ch_update_atts(&Chans[i]);

		if (ver < V8)
			LIBRETRO_US(dum); /* Chans[i].VolMix.DSPOut */
		ch_update_dsp_mix(&Chans[i]);

		LIBRETRO_US(Chans[i].AEG.val) ;
		LIBRETRO_US(Chans[i].AEG.state) ;
		/* (one of the four: the step function is looked up by it) */
		Chans[i].AEG.state = (_EG_state)((u32)Chans[i].AEG.state & 3);
		/* Only what the envelope steps by. ch_set_aeg_state() does one thing more
		 * for a channel that is released, which every idle one is: it clears
		 * the channel's key-on bit. A game sets that bit on the channels it
		 * wants and starts them all with one write later; loading a state
		 * between the two took the bit away, and the sound never started. */
		Chans[i].StepAEG = AEG_STEP_LUT[Chans[i].AEG.state];
		if (ver < V8)
		{
			LIBRETRO_US(dum); /* Chans[i].AEG.AttackRate */
			LIBRETRO_US(dum); /* Chans[i].AEG.Decay1Rate */
			LIBRETRO_US(dum); /* Chans[i].AEG.Decay2Rate */
			LIBRETRO_US(dum); /* Chans[i].AEG.Decay2Value */
			LIBRETRO_US(dum); /* Chans[i].AEG.ReleaseRate */
		}
		ch_update_aeg(&Chans[i]);
		LIBRETRO_US(Chans[i].FEG.value);
		LIBRETRO_US(Chans[i].FEG.state);
		Chans[i].FEG.state = (_EG_state)((u32)Chans[i].FEG.state & 3);
		if (ver >= V8)
		{
			LIBRETRO_US(Chans[i].FEG.prev1);
			LIBRETRO_US(Chans[i].FEG.prev2);
		}
		else
		{
			Chans[i].FEG.prev1 = 0;
			Chans[i].FEG.prev2 = 0;
		}
		/* The filter's carried fraction: not in states from before V16.
		 * From V17 the filter is another one: its two last outputs are
		 * 20-bit where they were 16, and the fraction is its own. */
		if (ver >= V16)
			LIBRETRO_US(Chans[i].FEG.fract);
		if (ver < V17)
		{
			Chans[i].FEG.prev1 <<= 4;
			Chans[i].FEG.prev2 <<= 4;
			Chans[i].FEG.fract = 0;
		}
		/* ch_set_feg_state() resets value/prev1/prev2 to the register level when the
		 * saved state is EG_Attack, which would discard the restored mid-attack
		 * filter-envelope state and desync audio after a savestate/runahead load.
		 * Preserve the values we just read across the state setup. */
		{
			u32 feg_value       = Chans[i].FEG.value;
			SampleType feg_prev1 = Chans[i].FEG.prev1;
			SampleType feg_prev2 = Chans[i].FEG.prev2;
			s32 feg_fract        = Chans[i].FEG.fract;
			ch_set_feg_state(&Chans[i], Chans[i].FEG.state);
			Chans[i].FEG.value = feg_value;
			Chans[i].FEG.prev1 = feg_prev1;
			Chans[i].FEG.prev2 = feg_prev2;
			Chans[i].FEG.fract = feg_fract;
		}
		ch_update_feg(&Chans[i]);
		if (ver < V8)
		{
			u8 dumu8;
			LIBRETRO_US(dumu8); /* Chans[i].step_stream_lut1 */
			LIBRETRO_US(dumu8); /* Chans[i].step_stream_lut2 */
			LIBRETRO_US(dumu8); /* Chans[i].step_stream_lut3 */
		}
		ch_update_stream_step(&Chans[i]);

		LIBRETRO_US(Chans[i].lfo.counter) ;
		if (ver < V8)
			LIBRETRO_US(dum); /* Chans[i].lfo.start_value */
		LIBRETRO_US(Chans[i].lfo.state) ;
		if (ver < V8)
		{
			u8 dumu8;
			LIBRETRO_US(dumu8); /* Chans[i].lfo.alfo */
			LIBRETRO_US(dumu8); /* Chans[i].lfo.alfo_shft */
			LIBRETRO_US(dumu8); /* Chans[i].lfo.plfo */
			LIBRETRO_US(dumu8); /* Chans[i].lfo.plfo_shft */
			LIBRETRO_US(dumu8); /* Chans[i].lfo.alfo_calc_lut */
			LIBRETRO_US(dumu8); /* Chans[i].lfo.plfo_calc_lut */
		}
		ch_update_lfo(&Chans[i], true);
		/* Between two samples the count is from 1 to where it starts: at 0
		 * the first step took it round to 4 billion, and the wave stood still
		 * for more than a day. */
		if (Chans[i].lfo.counter == 0 || Chans[i].lfo.counter > Chans[i].lfo.start_value)
			Chans[i].lfo.counter = Chans[i].lfo.start_value;
		LIBRETRO_US(Chans[i].enabled) ;
		if (ver < V8)
			LIBRETRO_US(dum); /* Chans[i].ChannelNumber */

	}
	/* the flags came from the state: the set of channels that are on follows them */
	ch_refresh_active();

	return true;
}

/* What a V21 state has that the ones before it have not: the low 8 bits of
 * each channel's place between two samples. The 10 above them are where they
 * always were, in channel_serialize(), and a state from before V21, read by
 * channel_unserialize(), leaves these 0. 64 bytes. */
void channel_serialize_v21(void **data, unsigned int *total_size)
{
	u8 step_lo[64];
	int i;

	for (i = 0; i < 64; i++)
		step_lo[i] = (u8)Chans[i].step;
	LIBRETRO_SA(step_lo, 64);
}

/* After channel_unserialize(), which has put the other 10 bits in place.
 * Any value of the 8 is a good one. */
void channel_unserialize_v21(void **data, unsigned int *total_size)
{
	u8 step_lo[64] = {0};
	int i;

	LIBRETRO_USA(step_lo, 64);
	for (i = 0; i < 64; i++)
		Chans[i].step = (Chans[i].step & (STEP_ONE - 256)) | step_lo[i];
}
