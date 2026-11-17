#include "types.h"
#include <new>
#include <streams/file_stream.h>
#include "maple_if.h"
#include "maple_helper.h"
#include "maple_devs.h"
#include "maple_cfg.h"
#include "jvs_board.h"
#include "libretro/pad_state.h"
#include "hw/pvr/spg.h"
#include "hw/naomi/naomi.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/naomi/naomi_periph.h"
#include "hw/naomi/printer.h"
#include "hw/naomi/midiffb.h"
#include "savewriter.h"
#include "vmu_beep.h"
#include <math.h>
#include <time.h>
#include <algorithm>
#include "paths.h"

#include <encodings/deflate.h>
#include <encodings/xxh32.h>

#define LOGJVS(...) DEBUG_LOG(JVS, __VA_ARGS__)

const char* maple_sega_controller_name = "Dreamcast Controller";
const char* maple_sega_twinstick_name = "Twin Stick";
const char* maple_sega_vmu_name = "Visual Memory";
const char* maple_ascii_stick_name = "ASCII STICK";
const char* maple_sega_kbd_name = "Emulated Dreamcast Keyboard";
const char* maple_sega_mouse_name = "Emulated Dreamcast Mouse";
const char* maple_sega_dreameye_name_1 = "Dreamcast Camera Flash  Devic";
const char* maple_sega_dreameye_name_2 = "Dreamcast Camera Flash LDevic";
const char* maple_sega_mic_name = "MicDevice for Dreameye";
const char* maple_sega_purupuru_name = "Puru Puru Pack";
const char* maple_sega_lightgun_name = "Dreamcast Gun";

const char* maple_sega_brand = "Produced By or Under License From SEGA ENTERPRISES,LTD.";

bool enable_naomi_15khz_dipswitch = false;
bool naomi_bios_multiregion = false;

enum MapleFunctionID
{
	MFID_0_Input       = 0x01000000, //DC Controller, Lightgun buttons, arcade stick .. stuff like that
	MFID_1_Storage     = 0x02000000, //VMU , VMS
	MFID_2_LCD         = 0x04000000, //VMU
	MFID_3_Clock       = 0x08000000, //VMU
	MFID_4_Mic         = 0x10000000, //DC Mic (, dreameye too ?)
	MFID_5_ARGun       = 0x20000000, //Artificial Retina gun ? seems like this one was never developed or smth -- I only remember of lightguns
	MFID_6_Keyboard    = 0x40000000, //DC Keyboard
	MFID_7_LightGun    = 0x80000000, //DC Lightgun
	MFID_8_Vibration   = 0x00010000, //Puru Puru Puur~~~
	MFID_9_Mouse       = 0x00020000, //DC Mouse
	MFID_10_StorageExt = 0x00040000, //Storage ? probably never used
	MFID_11_Camera     = 0x00080000, //DreamEye
};

enum MapleDeviceCommand
{
	MDC_DeviceRequest   = 0x01, //7 words.Note : Initialises device
	MDC_AllStatusReq    = 0x02, //7 words + device dependent ( seems to be 8 words)
	MDC_DeviceReset     = 0x03, //0 words
	MDC_DeviceKill      = 0x04, //0 words
	MDC_DeviceStatus    = 0x05, //Same as MDC_DeviceRequest ?
	MDC_DeviceAllStatus = 0x06, //Same as MDC_AllStatusReq ?

	//Various Functions
	MDCF_GetCondition   = 0x09, //FT
	MDCF_GetMediaInfo   = 0x0A, //FT,PT,3 pad
	MDCF_BlockRead      = 0x0B, //FT,PT,Phase,Block #
	MDCF_BlockWrite     = 0x0C, //FT,PT,Phase,Block #,data ...
	MDCF_GetLastError   = 0x0D, //FT,PT,Phase,Block #
	MDCF_SetCondition   = 0x0E, //FT,data ...
	MDCF_MICControl     = 0x0F, //FT,MIC data ...
	MDCF_ARGunControl   = 0x10, //FT,AR-Gun data ...

	MDC_JVSUploadFirmware = 0x80, // JVS bridge firmware
	MDC_JVSGetId		 = 0x82,
	MDC_JVSSelfTest		 = 0x84, // JVS Self Test
	MDC_JVSCommand		 = 0x86, // JVS I/O
};

enum MapleDeviceRV
{
	MDRS_JVSNone		 = 0x00, // No reply, used for multiple JVS I/O boards

	MDRS_DeviceStatus    = 0x05, //28 words
	MDRS_DeviceStatusAll = 0x06, //28 words + device dependent data
	MDRS_DeviceReply     = 0x07, //0 words
	MDRS_DataTransfer    = 0x08, //FT,depends on the command

	MDRE_UnknownFunction = 0xFE, //0 words
	MDRE_UnknownCmd      = 0xFD, //0 words
	MDRE_TransmitAgain   = 0xFC, //0 words
	MDRE_FileError       = 0xFB, //1 word, bitfield
	MDRE_LCDError        = 0xFA, //1 word, bitfield
	MDRE_ARGunError      = 0xF9, //1 word, bitfield

	MDRS_JVSSelfTestReply= 0x85, // JVS I/O SelfTest
	MDRS_JVSReply		 = 0x87, // JVS I/O
};




//fill in the info
void maple_device::Setup(u32 prt)
{
	maple_port = prt;
	bus_port = maple_GetPort(prt);
	bus_id = maple_GetBusId(prt);
	logical_port[0] = 'A' + bus_id;
	logical_port[1] = bus_port == 5 ? 'x' : '1' + bus_port;
	logical_port[2] = 0;
}
maple_device::~maple_device()
{
}

/*
	Base class with dma helpers and stuff
*/
struct maple_base: maple_device
{
	u8* dma_buffer_out;
	u32* dma_count_out;

	u8* dma_buffer_in;
	u32 dma_count_in;

	void w8(u8 data) { *((u8*)dma_buffer_out)=data;dma_buffer_out+=1;dma_count_out[0]+=1; }
	void w16(u16 data) { *((u16*)dma_buffer_out)=data;dma_buffer_out+=2;dma_count_out[0]+=2; }
	void w32(u32 data) { *(u32*)dma_buffer_out=data;dma_buffer_out+=4;dma_count_out[0]+=4; }

	void wptr(const void* src,u32 len)
	{
		u8* src8=(u8*)src;
		while(len--)
			w8(*src8++);
	}
	void wstr(const char* str,u32 len)
	{
		size_t ln=strlen(str);
		verify(len>=ln);
		len-=ln;
		while(ln--)
			w8(*str++);

		while(len--)
			w8(0x20);
	}
	
	u8 r8()	  { u8  rv=*((u8*)dma_buffer_in);dma_buffer_in+=1;dma_count_in-=1; return rv; }
	u16 r16() { u16 rv=*((u16*)dma_buffer_in);dma_buffer_in+=2;dma_count_in-=2; return rv; }
	// (a frame can be shorter than its command needs: the words it has not got read as 0, and the count does not go below 0)
	u32 r32() { if (dma_count_in<4) return 0; u32 rv=*(u32*)dma_buffer_in;dma_buffer_in+=4;dma_count_in-=4; return rv; }
	void rptr(void* dst, u32 len)
	{
		u8* dst8=(u8*)dst;
		while(len--)
			*dst8++=r8();
	}
	u32 r_count() { return dma_count_in; }

	u32 Dma(u32 Command,u32* buffer_in,u32 buffer_in_len,u32* buffer_out,u32& buffer_out_len)
	{
		dma_buffer_out=(u8*)buffer_out;
		dma_count_out=&buffer_out_len;

		dma_buffer_in=(u8*)buffer_in;
		dma_count_in=buffer_in_len;

		return dma(Command);
	}
	virtual u32 dma(u32 cmd)=0;

	virtual u32 RawDma(u32* buffer_in, u32 buffer_in_len, u32* buffer_out)
	{
		u32 command=buffer_in[0] &0xFF;
		//Recipient address
		u32 reci = (buffer_in[0] >> 8) & 0xFF;
		//Sender address
		u32 send = (buffer_in[0] >> 16) & 0xFF;
		u32 outlen = 0;
		u32 resp = Dma(command, &buffer_in[1], buffer_in_len - 4, &buffer_out[1], outlen);

		if (reci & 0x20)
			reci |= maple_GetAttachedDevices(bus_id);

		verify(u8(outlen/4)*4==outlen);
		buffer_out[0] = (resp <<0 ) | (send << 8) | (reci << 16) | ((outlen / 4) << 24);

		return outlen + 4;
	}
};

/*
	Sega Dreamcast Controller
	No error checking of any kind, but works just fine
*/
struct maple_sega_controller: maple_base
{
	virtual u32 get_capabilities() {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0xfe060f00;	// 4 analog axes (0-3) X Y A B Start U D L R
	}

	virtual u32 transform_kcode(u32 kcode) {
		return kcode | 0xF901;		// mask off DPad2, C, D and Z;
	}

	virtual u32 get_analog_axis(int index, const PlainJoystickState &pjs)
	{
		switch (index)
		{
		case 2:
			return pjs.joy[PJAI_X1];
		case 3:
			return pjs.joy[PJAI_Y1];
		case 0:
			return pjs.trigger[PJTI_R];		// Right trigger
		case 1:
			return pjs.trigger[PJTI_L];		// Left trigger
		default:
			return 0x80;					// unused
		}
	}

	virtual MapleDeviceType get_device_type()
	{
		return MDT_SegaController;
	}

	virtual const char *get_device_name()
	{
		return maple_sega_controller_name;
	}

	virtual const char *get_device_brand()
	{
		return maple_sega_brand;
	}

	virtual u32 dma(u32 cmd)
	{
		//printf("maple_sega_controller::dma Called 0x%X;Command %d\n",device_instance->port,Command);
		switch (cmd)
		{
		case MDC_DeviceRequest:
		case MDC_AllStatusReq:
			//caps
			//4
			w32(MFID_0_Input);

			//struct data
			//3*4
			w32(get_capabilities());
			w32( 0);
			w32( 0);

			//1	area code
			w8(0xFF);

			//1	direction
			w8(0);
			
			//30
			wstr(get_device_name(), 30);

			//60
			wstr(get_device_brand(), 60);

			//2
			w16(0x01AE);	// 43 mA

			//2
			w16(0x01F4);	// 50 mA

			if (cmd == MDC_AllStatusReq)
			{
				// what a controller adds when asked for all of its status: its maker's notes, 80 bytes
				const char *extra = "Version 1.010,1998/09/28,315-6211-AB   ,Analog Module : The 4th Edition.5/8  +DF";
				wptr(extra, strlen(extra));
				return MDRS_DeviceStatusAll;
			}
			return MDRS_DeviceStatus;

			//controller condition
		case MDCF_GetCondition:
			{
				PlainJoystickState pjs;
				pjs_init(&pjs);
				if (!SYSTEM_IS_NAOMI())
					mcfg_get_input(config, &pjs);

				//caps
				//4
				w32(MFID_0_Input);

				//state data
				//2 key code
				w16(transform_kcode(pjs.kcode));

				//triggers
				//1 R
				w8(get_analog_axis(0, pjs));
				//1 L
				w8(get_analog_axis(1, pjs));

				//joyx
				//1
				w8(get_analog_axis(2, pjs));
				//joyy
				//1
				w8(get_analog_axis(3, pjs));

				//not used on dreamcast
				//1
				w8(get_analog_axis(4, pjs));
				//1
				w8(get_analog_axis(5, pjs));
			}
			return MDRS_DataTransfer;

		case MDC_DeviceReset:
			return MDRS_DeviceReply;

		case MDC_DeviceKill:
			return MDRS_DeviceReply;

		default:
			INFO_LOG(MAPLE, "maple_sega_controller: Unknown maple command %d\n", cmd);
			return MDRE_UnknownCmd;
		}
	}
};

struct maple_atomiswave_controller : maple_sega_controller
{
	virtual u32 get_capabilities() override {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0xff663f00;	// 6 analog axes, X Y L2/D2(?) A B C Start U D L R
	}

	virtual u32 transform_kcode(u32 kcode) override {
		return kcode | AWAVE_TRIGGER_KEY;
	}

	virtual u32 get_analog_axis(int index, const PlainJoystickState &pjs) override {
		if (index < 2 || index > 5)
			return 0x80;
		index -= 2;
		u8 val;
		if (naomi_game_inputs != NULL && naomi_game_inputs->axes[index].name != NULL)
		{
			switch (naomi_game_inputs->axes[index].axis)
			{
			case 0:
				val = pjs.joy[0] - 0x80;
				break;
			case 1:
				val = pjs.joy[1] - 0x80;
				break;
			case 2:
				val = pjs.joy[2] - 0x80;
				break;
			case 3:
				val = pjs.joy[3] - 0x80;
				break;
			case 4:
				val = pjs.trigger[1];
				break;
			case 5:
				val = pjs.trigger[0];
				break;
			}
			if (naomi_game_inputs->axes[index].inverted)
				val = 0xff - val;
		}
		else
			val = pjs.joy[index] - 0x80;
		return val;
	}
};

/*
	Sega Twin Stick Controller
*/
struct maple_sega_twinstick : maple_sega_controller
{
	virtual u32 get_capabilities() override {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0xfefe0000;	// no analog axes, X Y A B D Start U/D/L/R U2/D2/L2/R2
	}

	virtual u32 transform_kcode(u32 kcode) override {
		return kcode | 0x0101;
	}

	virtual MapleDeviceType get_device_type() override {
		return MDT_TwinStick;
	}

	virtual u32 get_analog_axis(int index, const PlainJoystickState &pjs) override {
		return 0x80;
	}
		
	virtual const char *get_device_name() override {
		return maple_sega_twinstick_name;
	}
};


/*
	Ascii Stick (Arcade Stick)
*/
struct maple_ascii_stick : maple_sega_controller
{
	virtual u32 get_capabilities() override {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0xff070000;	// no analog axes, X Y Z A B C Start U/D/L/R
	}

	virtual u32 transform_kcode(u32 kcode) override {
		return kcode | 0xF800;
	}

	virtual MapleDeviceType get_device_type() override {
		return MDT_AsciiStick;
	}

	virtual u32 get_analog_axis(int index, const PlainJoystickState &pjs) override {
		return 0x80;
	}
		
	virtual const char *get_device_name() override {
		return maple_ascii_stick_name;
	}	
};

/*
	Sega Dreamcast Visual Memory Unit
	This is pretty much done (?)
*/


u8 vmu_default[] = {
		  0x78, 0x9c, 0xed, 0xdd, 0x5b, 0x88, 0x1c, 0x55, 0x1e, 0x07, 0xe0, 0xd3,
		  0x31, 0x6a, 0xf0, 0x3a, 0x0d, 0x0a, 0x5e, 0x18, 0x6a, 0x16, 0x02, 0x2e,
		  0xfb, 0x20, 0x68, 0x22, 0x2c, 0x28, 0x24, 0x21, 0x3d, 0xc2, 0x2e, 0xc6,
		  0x40, 0x26, 0xc3, 0x40, 0x2e, 0x90, 0x96, 0xec, 0xc8, 0x9a, 0xc4, 0x81,
		  0x6c, 0xa2, 0x20, 0x82, 0x11, 0xc1, 0x37, 0x41, 0x9f, 0x14, 0xf1, 0x61,
		  0x37, 0x64, 0xbc, 0x34, 0xb3, 0x17, 0xf0, 0xc1, 0x87, 0x0e, 0x9b, 0xb7,
		  0x55, 0x02, 0x3a, 0x19, 0x30, 0x8d, 0x0c, 0x1c, 0x82, 0xac, 0x17, 0x76,
		  0x59, 0x30, 0x77, 0xb5, 0x92, 0xce, 0xb1, 0xaa, 0x27, 0xc6, 0x98, 0x98,
		  0x68, 0xb2, 0x31, 0x15, 0x93, 0xef, 0x2b, 0x8a, 0x9a, 0xff, 0xef, 0xd4,
		  0xa9, 0x73, 0xe8, 0x3a, 0xf5, 0xd4, 0xc5, 0x74, 0x08, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd5, 0xfa, 0xf3, 0xdc, 0x2f, 0x47,
		  0x52, 0xfa, 0xae, 0xee, 0x7b, 0xfe, 0xf6, 0xaf, 0xe6, 0x0e, 0x84, 0xd0,
		  0x78, 0x20, 0x84, 0x4d, 0xa3, 0x95, 0x4d, 0x0b, 0x00, 0x7e, 0x31, 0xfe,
		  0xf8, 0xfb, 0x87, 0x6f, 0xa8, 0x7a, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0xc0, 0x95, 0xe1, 0x77, 0x9f, 0xfc, 0x69, 0xb2, 0x3c, 0xde, 0x7a, 0xed,
		  0x9c, 0xf0, 0xef, 0xa1, 0xe9, 0x63, 0x55, 0xcf, 0x07, 0x00, 0x00, 0x00,
		  0x00, 0x2e, 0x07, 0x0b, 0xcf, 0x92, 0x5d, 0x5f, 0x7b, 0x69, 0xf0, 0xc7,
		  0xce, 0xfd, 0x29, 0xd7, 0x2b, 0xc5, 0x3b, 0x47, 0x36, 0xbd, 0x58, 0xfb,
		  0xe7, 0x55, 0xc7, 0x8a, 0x6d, 0x7b, 0x2d, 0x84, 0x05, 0x0b, 0x6e, 0xfe,
		  0xed, 0x6d, 0x3f, 0x75, 0x92, 0x5c, 0x14, 0x0b, 0x65, 0x57, 0x5c, 0x56,
		  0xe5, 0xd8, 0xb2, 0x6a, 0xb3, 0x4b, 0x6d, 0x3e, 0xb2, 0x6a, 0xb2, 0x2a,
		  0xc7, 0x96, 0xb9, 0xef, 0xb2, 0xea, 0xb3, 0x2a, 0xc7, 0x96, 0xb9, 0xef,
		  0xb2, 0x8b, 0x9f, 0x5d, 0x6a, 0xf3, 0x91, 0x55, 0x93, 0x55, 0x39, 0xb6,
		  0xcc, 0x7d, 0x97, 0x55, 0x9f, 0x55, 0x39, 0xb6, 0xac, 0xda, 0xfb, 0xfe,
		  0x7f, 0x6a, 0x8c, 0x8d, 0x6d, 0x78, 0x64, 0x7d, 0x73, 0xed, 0x1f, 0x4e,
		  0x04, 0x1b, 0x9b, 0x8f, 0xac, 0xdf, 0xbc, 0x69, 0x6c, 0xe3, 0xb7, 0xc1,
		  0xd0, 0xba, 0xb1, 0x8d, 0x63, 0x8f, 0x37, 0x37, 0x9d, 0xe8, 0xb2, 0xbc,
		  0xa8, 0xd7, 0x7d, 0xef, 0x22, 0xbf, 0x2e, 0xf6, 0xeb, 0x8a, 0xbd, 0x16,
		  0x66, 0xfe, 0xd1, 0xd7, 0x96, 0x0d, 0xff, 0x69, 0xbc, 0xfc, 0xfc, 0xdb,
		  0xc5, 0x94, 0x9f, 0x5a, 0xfc, 0xdf, 0xa2, 0xe3, 0xb3, 0xb3, 0x26, 0xff,
		  0x75, 0xf2, 0xf9, 0x0b, 0x7f, 0x60, 0x22, 0xb2, 0xca, 0xd7, 0x02, 0xf0,
		  0x0b, 0xf3, 0x41, 0xd5, 0x13, 0xa0, 0x22, 0x7d, 0x67, 0x6a, 0x98, 0xf3,
		  0xa3, 0x5d, 0x6b, 0x17, 0x74, 0x22, 0xe7, 0xed, 0xb3, 0xbb, 0xa7, 0xef,
		  0x5f, 0x74, 0x7a, 0x9c, 0xae, 0x3f, 0x2d, 0xfa, 0xcd, 0x45, 0x98, 0x0d,
		  0x17, 0xdb, 0x19, 0x57, 0xf0, 0xec, 0x62, 0x9f, 0x75, 0xa2, 0x38, 0x96,
		  0xce, 0x55, 0xaf, 0xdf, 0xb9, 0x77, 0x3b, 0xa3, 0x2c, 0xec, 0x3d, 0xb7,
		  0x0e, 0x0b, 0xce, 0x7f, 0xf0, 0x67, 0x8a, 0xa9, 0x6f, 0x39, 0xfb, 0xe7,
		  0x76, 0xd3, 0x85, 0xf8, 0xf0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0xb8, 0x24, 0xa4, 0x6d, 0x31, 0xc6, 0x3d, 0xdd, 0x5d, 0x77, 0x64, 0xbb,
		  0xfa, 0xa7, 0xba, 0xf9, 0x93, 0x69, 0xe7, 0xea, 0x18, 0xf3, 0x38, 0x55,
		  0xef, 0xee, 0xea, 0xcf, 0x52, 0xeb, 0xe9, 0x94, 0x2f, 0x89, 0x71, 0xd5,
		  0x5f, 0x9b, 0x59, 0x77, 0xaa, 0x9e, 0x26, 0x27, 0x26, 0x77, 0x1c, 0xdd,
		  0xb6, 0x3a, 0x4e, 0x77, 0x9b, 0x59, 0x7c, 0x21, 0x4b, 0xf9, 0xe4, 0x91,
		  0x1d, 0x69, 0xe7, 0xb6, 0x46, 0xa3, 0xf1, 0xab, 0xb4, 0x7c, 0x38, 0x5b,
		  0x31, 0x91, 0x8e, 0xb4, 0x53, 0xda, 0xd9, 0x6a, 0xb5, 0x26, 0xf3, 0xd6,
		  0x44, 0x9a, 0x48, 0xa9, 0xdd, 0xde, 0x71, 0xf2, 0x97, 0xd2, 0x47, 0xda,
		  0xed, 0xf6, 0xf7, 0xcb, 0xd3, 0xdf, 0xdb, 0x98, 0xd3, 0x7b, 0x0f, 0xb4,
		  0x7c, 0xb5, 0x24, 0xbf, 0x2d, 0x84, 0xc7, 0x76, 0x7f, 0xfa, 0x79, 0x79,
		  0xac, 0xcd, 0x0a, 0x61, 0xd9, 0xa2, 0xc7, 0x9f, 0x68, 0x6e, 0x3c, 0xf9,
		  0xdc, 0x5b, 0x06, 0xeb, 0xb3, 0x9f, 0xdb, 0xfa, 0xca, 0xab, 0xff, 0xb8,
		  0x7a, 0xd1, 0xa3, 0xd7, 0x5c, 0x88, 0x8f, 0x63, 0xf6, 0x59, 0xda, 0x2e,
		  0xcb, 0xdf, 0xcc, 0x7b, 0x68, 0x69, 0x63, 0xe9, 0xd0, 0xd0, 0xc0, 0xc8,
		  0xa2, 0x65, 0x03, 0x3d, 0xcb, 0x06, 0x17, 0x2f, 0x5d, 0xd6, 0x18, 0x58,
		  0xfa, 0xe0, 0xc0, 0xa9, 0x2d, 0x33, 0x4e, 0xed, 0x3f, 0xf3, 0x0a, 0xd0,
		  0x6b, 0xb5, 0xb5, 0x4f, 0xfc, 0xf0, 0xf5, 0xbf, 0x4e, 0x07, 0xd3, 0x9b,
		  0xf9, 0xe8, 0xe1, 0xb5, 0x87, 0x87, 0x0e, 0xdd, 0x7b, 0xf0, 0xde, 0x03,
		  0xf7, 0x1c, 0x2c, 0xb6, 0x03, 0xd9, 0xde, 0x81, 0xfd, 0x7d, 0xfb, 0xc2,
		  0xbe, 0xb0, 0xb7, 0xdd, 0x7e, 0x67, 0xb8, 0xb0, 0xbc, 0xb1, 0x78, 0xde,
		  0xbc, 0xf9, 0x2b, 0xdb, 0xc7, 0xcb, 0xe1, 0xe1, 0xc1, 0x32, 0x29, 0x83,
		  0xcd, 0x2b, 0x8a, 0xe5, 0x55, 0x2e, 0xa9, 0x56, 0x6b, 0x7c, 0xde, 0x60,
		  0x7b, 0xfb, 0xf0, 0xbb, 0x1f, 0xd7, 0xeb, 0x33, 0xcb, 0x27, 0x1f, 0x9f,
		  0xbf, 0x72, 0xfb, 0xaa, 0xb4, 0xa1, 0x96, 0xba, 0x31, 0xee, 0x2e, 0x96,
		  0x71, 0x3e, 0x74, 0xff, 0xd1, 0x9d, 0xdd, 0x58, 0x2b, 0xca, 0x38, 0x51,
		  0xae, 0xeb, 0xf1, 0xfb, 0x52, 0x2b, 0x76, 0x7a, 0xf5, 0x92, 0x5e, 0x3d,
		  0x2f, 0xfd, 0x3d, 0x76, 0xea, 0x65, 0x3d, 0x5d, 0xec, 0x31, 0x9f, 0x9f,
		  0x26, 0xe2, 0x68, 0x56, 0xfe, 0xb9, 0xa7, 0x57, 0x37, 0xd2, 0x1b, 0x71,
		  0x4d, 0x51, 0x77, 0x46, 0xdf, 0x5f, 0x31, 0x53, 0x8f, 0xc7, 0x62, 0xad,
		  0x97, 0xe7, 0x7c, 0x78, 0xbc, 0xee, 0x34, 0xbb, 0xb1, 0xd3, 0x4c, 0x31,
		  0xae, 0xd8, 0x13, 0xf3, 0xc1, 0x34, 0x5e, 0x34, 0x75, 0x46, 0x6b, 0x33,
		  0x33, 0x28, 0xea, 0xad, 0xcd, 0x7a, 0xbd, 0x2f, 0xf4, 0x86, 0x98, 0x58,
		  0x9d, 0x8f, 0xa4, 0xad, 0x59, 0xfa, 0xa2, 0x5e, 0xcf, 0xca, 0xba, 0x68,
		  0x1f, 0x49, 0xaf, 0x17, 0x3d, 0x53, 0x3c, 0xae, 0xec, 0x5f, 0x1e, 0xa7,
		  0x5b, 0x6f, 0x95, 0x87, 0xd1, 0xff, 0x0d, 0x17, 0xf5, 0x9e, 0x38, 0x5a,
		  0x5c, 0xa3, 0x3b, 0x5a, 0x8c, 0x92, 0x95, 0xf5, 0xee, 0xce, 0x9a, 0x6c,
		  0x5f, 0x16, 0x9b, 0xb1, 0xd3, 0x97, 0xf5, 0xce, 0x7f, 0xbd, 0xd6, 0x9d,
		  0x2a, 0xea, 0x2f, 0x3a, 0xbb, 0x7a, 0xed, 0xf1, 0x70, 0xad, 0x78, 0x58,
		  0x3f, 0xaa, 0x67, 0x71, 0x2a, 0xf5, 0xea, 0xbf, 0xd5, 0xe3, 0x96, 0xfe,
		  0x4e, 0xbd, 0xbf, 0xd3, 0xcc, 0x7b, 0xf5, 0x87, 0xf5, 0xb8, 0xa6, 0xff,
		  0x2f, 0x7b, 0xfb, 0x3b, 0x59, 0x51, 0x17, 0xcf, 0x7b, 0x27, 0xef, 0x2f,
		  0x1e, 0xee, 0xe2, 0x79, 0x4f, 0xf9, 0xc8, 0xcf, 0xb9, 0xb6, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0xe0, 0x72, 0x73, 0x34, 0xd9, 0x2e, 0xef, 0x2d, 0x4f, 0xfb, 0xc2, 0xfe,
		  0x70, 0x20, 0x1c, 0x0c, 0x87, 0xc2, 0xe1, 0xf0, 0x65, 0xf8, 0x2a, 0x7c,
		  0x1d, 0xf2, 0x70, 0x24, 0x1c, 0x0d, 0x79, 0xd1, 0x36, 0x7c, 0x8a, 0x5a,
		  0xf9, 0xc3, 0x7a, 0x67, 0x59, 0x2f, 0xb7, 0xbf, 0x52, 0xbf, 0x2b, 0x84,
		  0x81, 0x13, 0xbf, 0x93, 0x98, 0x7a, 0xfb, 0xb1, 0x50, 0x0b, 0xdd, 0x70,
		  0x63, 0xd8, 0x1c, 0xde, 0x0b, 0x59, 0x91, 0x6c, 0xf9, 0xb9, 0x97, 0x2d,
		  0x17, 0xc8, 0x37, 0x7a, 0x6b, 0xe6, 0x6e
};

/* The default VMU image is kept as a zlib stream; inflate it into @dst,
 * which must hold exactly the uncompressed size. */
static bool vmu_inflate_default(u8 *dst, size_t dst_len)
{
   void *inf;
   size_t wr = 0;
   int rv;

   if (!(inf = rinflate_new(15)))
      return false;
   rinflate_set_in(inf, vmu_default, sizeof(vmu_default));
   rinflate_set_out(inf, dst, dst_len);
   rv = rinflate_process(inf, NULL, &wr);
   rinflate_free(inf);
   return rv == RDEFLATE_PROCESS_END && wr == dst_len;
}

/* How the writes a device hands to the save writer are getting on. */
struct MapleSaveState
{
	retro_atomic_int_t failed;	// set by the writer: a write did not go through
	u32 wait;					// frames until the next try, after one that failed
	u32 again;					// how long the last such wait was

	MapleSaveState() : wait(0), again(0)
	{
		retro_atomic_int_init(&failed, 0);
	}
	/* Whether a write has failed since this was last asked: 0 if not, 1
	 * the first time, 2 every time after. There is a while before the
	 * next try - a second, then twice as long each time, up to a minute:
	 * a file that cannot be written at all (no such directory, a disk
	 * that is full) is not hammered at, and whoever logs the failure does
	 * so the first time only. */
	int Failed()
	{
		if (!retro_atomic_load_acquire_int(&failed) || !retro_atomic_exchange_int(&failed, 0))
			return 0;
		again = again == 0 ? 60 : again >= 1800 ? 3600 : again * 2;
		wait = again;
		return again == 60 ? 1 : 2;
	}
	// Whether it is still too soon after a failure to try again
	bool Waiting()
	{
		if (wait == 0)
			return false;
		wait--;
		return true;
	}
};

#include "hw/aica/aica_if.h"

/* A date as a memory card gives it, from the console's clock (seconds since
 * 1950-01-01, local time): year (two bytes, low first), month, day, hour,
 * minute, second, and a byte that is 0. */
static void vmu_clock_date(u32 rtc, u8 *buf)
{
	const u32 secs = rtc % 86400;
	// days since 0000-03-01 of the Gregorian calendar, in 400-year cycles
	const u32 days = rtc / 86400 + 712163;
	const u32 era = days / 146097;
	const u32 doe = days - era * 146097;
	const u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const u32 mp = (5 * doy + 2) / 153;
	const u32 day = doy - (153 * mp + 2) / 5 + 1;
	const u32 month = mp < 10 ? mp + 3 : mp - 9;
	const u32 year = yoe + era * 400 + (month <= 2);

	buf[0] = year % 256;
	buf[1] = year / 256;
	buf[2] = month;
	buf[3] = day;
	buf[4] = secs / 3600;
	buf[5] = secs / 60 % 60;
	buf[6] = secs % 60;
	buf[7] = 0;
}

struct maple_sega_vmu: maple_base
{
	RFILE* file;
	/* Bytes of flash_data changed since the last flush: [dirty_lo, dirty_hi) */
	u32 dirty_lo;
	u32 dirty_hi;
	/* Bytes where the card is not what its file has because a state was
	 * loaded: [loaded_lo, loaded_hi). See maple_unserialize(). */
	u32 loaded_lo = 0;
	u32 loaded_hi = 0;
	MapleSaveState save_state;	// of the writes handed over: see FlushSave()
	u8 flash_data[128*1024];
	u8 lcd_data[192];
	u8 lcd_data_decoded[VMU_SCREEN_WIDTH*VMU_SCREEN_HEIGHT];
	std::string path;	// the card's file
	
	virtual MapleDeviceType get_device_type()
	{
		return MDT_SegaVMU;
	}

	virtual bool maple_serialize(void **data, unsigned int *total_size)
	{
		LIBRETRO_SA(flash_data,128*1024);
		LIBRETRO_SA(lcd_data,192);
		LIBRETRO_SA(lcd_data_decoded,48*32);
		return true ;
	}
	virtual bool maple_unserialize(void **data, unsigned int *total_size)
	{
		/* What is on the card is about to be what the state says, and the
		 * file is still what it was. Loading a state does not write to the
		 * file: where the two now differ is noted, and goes to the file
		 * with the next thing the game writes to the card - or the file
		 * would be the card in part and what it was before in part, a
		 * broken file system. Whatever was waiting to be written belongs to
		 * the card as it was: out it goes first.
		 *
		 * (The whole card used to be written then, and every load counted
		 * as one that changed the card. A frontend's run-ahead loads a
		 * state every frame, and it is the card as it is but for what the
		 * game wrote in the last frame: usually nothing, and then there is
		 * nothing to do; while a game saves, a block or two, where it was
		 * 128K a frame.) */
		u32 lo = 0, hi = sizeof(flash_data);

		if (ra_unserialize_has(*data, sizeof(flash_data)))
		{
			const u8 * const from = (const u8 *)*data;

			// (by the block: that is what a game writes)
			while (lo < hi && memcmp(&flash_data[lo], &from[lo], 512) == 0)
				lo += 512;
			while (hi > lo && memcmp(&flash_data[hi - 512], &from[hi - 512], 512) == 0)
				hi -= 512;
		}
		if (lo < hi)
		{
			save_state.wait = 0;
			FlushSave();
			if (dirty_lo < dirty_hi)
			{
				// (not taken: it waits with the rest)
				lo = std::min(lo, dirty_lo);
				hi = std::max(hi, dirty_hi);
				dirty_lo = 0;
				dirty_hi = 0;
			}
			if (loaded_lo < loaded_hi)
			{
				lo = std::min(lo, loaded_lo);
				hi = std::max(hi, loaded_hi);
			}
			loaded_lo = lo;
			loaded_hi = hi;
		}
		LIBRETRO_USA(flash_data,128*1024);
		LIBRETRO_USA(lcd_data,192);
		LIBRETRO_USA(lcd_data_decoded,48*32);
		/* The two cards of a controller show on one screen, which has the
		 * picture the game last drew on either. The second card's, after
		 * the first's, only if it has ever been drawn on: or a card the
		 * game does not draw on puts its blank screen in place of the
		 * other's picture at every load. */
		bool drawn = bus_port == 0;
		for (u32 i = 0; !drawn && i < sizeof(lcd_data); i++)
			drawn = lcd_data[i] != 0;
		if (drawn)
			vmu_lcd_publish(bus_id, lcd_data_decoded);
		return true ;
	}
	virtual void OnSetup()
	{
		memset(flash_data,0,sizeof(flash_data));
		memset(lcd_data,0,sizeof(lcd_data));
		dirty_lo = 0;
		dirty_hi = 0;
      std::string apath = get_writable_vmu_path(logical_port);
      path = apath;

		vmu_screen_params[bus_id].vmu_lcd_screen = lcd_data_decoded ;
		// What the renderer shows until the game draws: the screen as it is now
		vmu_lcd_publish(bus_id, lcd_data_decoded);

		INFO_LOG(MAPLE, "Initializing VMU data...\n");
		verify(vmu_inflate_default(flash_data, sizeof(flash_data)));

		file=filestream_open(apath.c_str(), RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING, RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if (!file)
		{
			INFO_LOG(MAPLE, "Unable to open VMU save file \"%s\", creating new file\n",apath.c_str());
			file=filestream_open(apath.c_str(), RETRO_VFS_FILE_ACCESS_READ_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
			if (file) {
				filestream_write(file, flash_data, sizeof(flash_data));
				filestream_seek(file,0,RETRO_VFS_SEEK_POSITION_START);
			} else {
				INFO_LOG(MAPLE, "Unable to create VMU!\n");
			}
		}

		if (!file)
		{
			WARN_LOG(MAPLE, "Failed to create VMU save file \"%s\"\n", apath.c_str());
		}
		else
		{
			filestream_read(file,flash_data,sizeof(flash_data));
			NOTICE_LOG(MAPLE, "Loaded VMU from file \"%s\"\n", apath.c_str());
		}
	}
	/* What the game wrote to the card since the last time goes to the
	 * card's file: handed to the writer's thread (savewriter.h), which
	 * does the waiting for the disk. The file is the writer's from the
	 * first hand-over on.
	 *
	 * A write that fails is found out later: the whole card is then
	 * written again, since which part of the file is not what it should
	 * be is no longer known, a second or so later and for as long as it
	 * goes on failing. It used to be forgotten - what was to be written
	 * counted as written whatever the write said. */
	virtual void FlushSave()
	{
		if (const int failed = save_state.Failed())
		{
			if (failed == 1)
				WARN_LOG(MAPLE, "VMU %s: a write to its file failed. The card will be written again\n", logical_port);
			dirty_lo = 0;
			dirty_hi = sizeof(flash_data);
		}
		if (dirty_lo >= dirty_hi || save_state.Waiting())
			return;
		if (file)
		{
			// (not taken: too much is waiting to be written. Next frame.)
			if (!save_writer_put(file, dirty_lo, &flash_data[dirty_lo], dirty_hi - dirty_lo, &save_state.failed))
				return;
		}
		else
			INFO_LOG(MAPLE, "Failed to save VMU %s data\n", logical_port);
		dirty_lo = 0;
		dirty_hi = 0;
	}
	virtual ~maple_sega_vmu()
	{
		save_state.wait = 0;
		FlushSave();
		if (save_state.wait != 0)
		{
			// (that found a write had failed, and put the next one off: now, all the same)
			save_state.wait = 0;
			FlushSave();
		}
		// the writer has the file, and the flag that is this card's
		save_writer_drain();
		if (file && save_state.Failed())
			WARN_LOG(MAPLE, "VMU %s: the last write to its file failed\n", logical_port);
		if (file) filestream_close(file);
	}
	virtual u32 dma(u32 cmd)
	{
		//printf("maple_sega_vmu::dma Called for port %d:%d, Command %d\n", bus_id, bus_port, cmd);
		switch (cmd)
		{
		case MDC_DeviceRequest:
		case MDC_AllStatusReq:
			//caps
			//4
			w32(MFID_1_Storage | MFID_2_LCD | MFID_3_Clock);

			//struct data
			//3*4
			w32( 0x403f7e7e); // for clock
			w32( 0x00100500); // for LCD
			w32( 0x00410f00); // for storage
			//1  area code
			w8(0xFF);
			//1  direction
			w8(0);
			//30
			wstr(maple_sega_vmu_name,30);

			//60
			wstr(maple_sega_brand,60);

			//2
			w16(0x007c);	// 12.4 mA

			//2
			w16(0x0082);	// 13 mA

			if (cmd == MDC_AllStatusReq)
			{
				// and a memory card's
				const char *extra = "Version 1.005,1999/04/15,315-6208-03,SEGA Visual Memory System BIOS Produced by ";
				wptr(extra, strlen(extra));
				return MDRS_DeviceStatusAll;
			}
			return MDRS_DeviceStatus;

			//in[0] is function used
			//out[0] is function used
		case MDCF_GetMediaInfo:
			{
				u32 function=r32();
				switch(function)
				{
				case MFID_1_Storage:
					DEBUG_LOG(MAPLE, "VMU %s GetMediaInfo storage\n", logical_port);
					w32(MFID_1_Storage);

					if (*(u16*)&flash_data[0xFF * 512 + 0x40] != 0xFF)
					{
						// Unformatted state: return predetermined media information
						//total_size;
						w16(0xff);
						//partition_number;
						w16(0);
						//system_area_block;
						w16(0xFF);
						//fat_area_block;
						w16(0xfe);
						//number_fat_areas_block;
						w16(1);
						//file_info_block;
						w16(0xfd);
						//number_info_blocks;
						w16(0xd);
						//volume_icon;
						w8(0);
						//reserved1;
						w8(0);
						//save_area_block;
						w16(0xc8);
						//number_of_save_blocks;
						w16(0x1f);
						//reserverd0 (something for execution files?)
						w32(0);
					}
					else
					{
						// Get data from the vmu system area (block 0xFF)
						wptr(flash_data + 0xFF * 512 + 0x40, 24);
					}
					return MDRS_DataTransfer;//data transfer

				case MFID_2_LCD:
					{
						u32 pt=r32();
						if (pt!=0)
						{
							INFO_LOG(MAPLE, "VMU: MDCF_GetMediaInfo -> bad input |%08X|, returning MDRE_UnknownCmd\n", pt);
							return MDRE_UnknownCmd;
						}
						else
						{
							w32(MFID_2_LCD);

							w8(47);             //X dots -1
							w8(31);             //Y dots -1
							w8(((1)<<4) | (0)); //1 Color, 0 contrast levels
							w8(2);              //Padding
							
							return MDRS_DataTransfer;
						}
					}

				default:
					INFO_LOG(MAPLE, "VMU: MDCF_GetMediaInfo -> Bad function used |%08X|, returning -2\n", function);
					return MDRE_UnknownFunction;//bad function
				}
			}
			break;

		case MDCF_BlockRead:
			{
				u32 function=r32();
				switch(function)
				{
				case MFID_1_Storage:
					{
						w32(MFID_1_Storage);
						u32 xo=r32();
						u32 Block = (SWAP32(xo))&0xffff;
						w32(xo);

						if (Block>255)
						{
							DEBUG_LOG(MAPLE, "Block read : %d\n", Block);
							DEBUG_LOG(MAPLE, "BLOCK READ ERROR\n");
							Block&=255;
						}
						else
							DEBUG_LOG(MAPLE, "VMU %s block read: Block %d addr %x len %d\n", logical_port, Block, Block*512, 512);
						wptr(flash_data+Block*512,512);
					}
					return MDRS_DataTransfer;//data transfer

				case MFID_2_LCD:
					DEBUG_LOG(MAPLE, "VMU %s read LCD\n", logical_port);
					w32(MFID_2_LCD);
					w32(r32()); // mnn ?
					// (the screen's own bytes: it was the first 192 of the card's memory)
					wptr(lcd_data,192);

					return MDRS_DataTransfer;//data transfer

				case MFID_3_Clock:
					if (r32()!=0)
					{
						INFO_LOG(MAPLE, "VMU: Block read: MFID_3_Clock : invalid params\n");
						return MDRE_TransmitAgain; //invalid params
					}
					else
					{
						w32(MFID_3_Clock);

						/* The date and time the console's clock says. (It
						 * was the host's: a game reading it got another
						 * answer when a frame was run again from a save
						 * state, which run-ahead and netplay do.) */
						u8* timebuf=dma_buffer_out;
						u8 date[8];
						vmu_clock_date(RealTimeClock, date);
						wptr(date, sizeof(date));

						DEBUG_LOG(MAPLE, "VMU: CLOCK Read-> datetime is %04d/%02d/%02d ~ %02d:%02d:%02d!\n",
								timebuf[0] + timebuf[1] * 256,
								timebuf[2],
								timebuf[3],
								timebuf[4],
								timebuf[5],
								timebuf[6]);

						return MDRS_DataTransfer;//transfer reply ...
					}

				default:
					INFO_LOG(MAPLE, "VMU: cmd MDCF_BlockRead -> Bad function |%08X| used, returning -2\n", function);
					return MDRE_UnknownFunction;//bad function
				}
			}
			break;

		case MDCF_BlockWrite:
			{
				switch(r32())
				{
					case MFID_1_Storage:
					{
						u32 bph=r32();
						u32 Block = (SWAP32(bph))&0xffff;
						u32 Phase = ((SWAP32(bph))>>16)&0xff;
						u32 write_adr=Block*512+Phase*(512/4);
						u32 write_len=r_count();
						DEBUG_LOG(MAPLE, "VMU %s block write: Block %d Phase %d addr %x len %d\n", logical_port, Block, Phase, write_adr, write_len);
						if (write_adr + write_len > sizeof(flash_data))
						{
							INFO_LOG(MAPLE, "Failed to write VMU %s: overflow\n", logical_port);
							return MDRE_TransmitAgain; //invalid params
						}
						rptr(&flash_data[write_adr],write_len);

						/* Written out by FlushSave() between frames */
						if (dirty_lo >= dirty_hi)
						{
							dirty_lo = write_adr;
							dirty_hi = write_adr + write_len;
						}
						else
						{
							if (write_adr < dirty_lo)
								dirty_lo = write_adr;
							if (write_adr + write_len > dirty_hi)
								dirty_hi = write_adr + write_len;
						}
						if (loaded_lo < loaded_hi)
						{
							// the first write after a state was loaded that changed the card: see maple_unserialize()
							dirty_lo = std::min(dirty_lo, loaded_lo);
							dirty_hi = std::max(dirty_hi, loaded_hi);
							loaded_lo = 0;
							loaded_hi = 0;
						}
						return MDRS_DeviceReply;//just ko
					}

					case MFID_2_LCD:
					{
						DEBUG_LOG(MAPLE, "VMU %s LCD write\n", logical_port);
						r32();
						if (r_count() < 192)
						{
							INFO_LOG(MAPLE, "VMU %s LCD write invalid params: rcount %d\n", logical_port, r_count());
							return MDRE_TransmitAgain;	//invalid params
						}
						rptr(lcd_data,192);

						u8 white=0xff,black=0x00;

						for(int y=0;y<32;++y)
						{
							u8* dst=lcd_data_decoded+y*48;
							u8* src=lcd_data+6*y+5;
							for(int x=0;x<6;++x)
							{
								u8 col=*src--;
								for(int l=0;l<8;l++)
								{
									*dst++=col&1?black:white;
									col>>=1;
								}
							}
						}
						mcfg_set_image(config, lcd_data_decoded);
						push_vmu_screen(lcd_data_decoded);
						return  MDRS_DeviceReply;//just ko
					}

					case MFID_3_Clock:
						if (r32()!=0 || r_count()!=8)
						{
							INFO_LOG(MAPLE, "VMU %s clock write invalid params: rcount %d\n", logical_port, r_count());
							return MDRE_TransmitAgain;	//invalid params ...
						}
						else
						{
							u8 timebuf[8];
							rptr(timebuf,8);
							DEBUG_LOG(MAPLE, "VMU: CLOCK Write-> datetime is %04d/%02d/%02d ~ %02d:%02d:%02d! Nothing set tho ...\n",
									timebuf[0]+timebuf[1]*256,timebuf[2],timebuf[3],timebuf[4],timebuf[5],timebuf[6]);
							return  MDRS_DeviceReply;//ok !
						}

					default:
						INFO_LOG(MAPLE, "VMU: command MDCF_BlockWrite -> Bad function used, returning MDRE_UnknownFunction\n");
						return  MDRE_UnknownFunction;//bad function
				}
			}
			break;

		case MDCF_GetLastError:
			return MDRS_DeviceReply;//just ko

		case MDCF_SetCondition:
			{
				switch(r32())
				{
				case MFID_3_Clock:
					{
						/* The alarm: its period, then its on time, in
						 * the first two bytes (the other two are a
						 * second alarm, not emulated). */
						u32 bp=r32();
						if (bp)
						{
							INFO_LOG(MAPLE, "BEEP : %08X\n", bp);
						}
						vmu_beep_set(bp & 0xFF, (bp >> 8) & 0xFF);
						return  MDRS_DeviceReply;//just ko
					}
					break;

				default:
					{
						INFO_LOG(MAPLE, "VMU: command MDCF_SetCondition -> Bad function used, returning MDRE_UnknownFunction\n");
						return MDRE_UnknownFunction;//bad function
					}
					break;
				}
			}

		case MDC_DeviceReset:
			vmu_beep_set(0, 0);
			return MDRS_DeviceReply;

		case MDC_DeviceKill:
			vmu_beep_set(0, 0);
			return MDRS_DeviceReply;

		default:
			DEBUG_LOG(MAPLE, "Unknown MAPLE COMMAND %d\n", cmd);
			return MDRE_UnknownCmd;
		}
	}	
};


/* Whether a memory card would now be read from another file than the one it
 * was read from: the settings say which, and they may have changed. */
bool maple_vmu_file_changed(maple_device *dev)
{
	if (dev->get_device_type() != MDT_SegaVMU)
		return false;
	maple_sega_vmu *vmu = (maple_sega_vmu *)dev;
	return vmu->path != get_writable_vmu_path(vmu->logical_port);
}

struct maple_microphone: maple_base
{
	u8 micdata[SIZE_OF_MIC_DATA];

	virtual MapleDeviceType get_device_type()
	{
		return MDT_Microphone;
	}

	virtual bool maple_serialize(void **data, unsigned int *total_size)
	{
		LIBRETRO_SA(micdata,SIZE_OF_MIC_DATA);
		return true ;
	}
	virtual bool maple_unserialize(void **data, unsigned int *total_size)
	{
		LIBRETRO_USA(micdata,SIZE_OF_MIC_DATA);
		return true ;
	}
	virtual void OnSetup()
	{
		memset(micdata,0,sizeof(micdata));
	}

	virtual u32 dma(u32 cmd)
	{
		//printf("maple_microphone::dma Called 0x%X;Command %d\n",this->maple_port,cmd);
		//LOGD("maple_microphone::dma Called 0x%X;Command %d\n",this->maple_port,cmd);

		switch (cmd)
		{
		case MDC_DeviceRequest:
		case MDC_AllStatusReq:
			DEBUG_LOG(MAPLE, "maple_microphone::dma MDC_DeviceRequest\n");
			//this was copied from the controller case with just the id and name replaced!

			//caps
			//4
			w32(MFID_4_Mic);

			//struct data
			//3*4
			w32( 0xfe060f00);
			w32( 0);
			w32( 0);

			//1	area code
			w8(0xFF);

			//1	direction
			w8(0);
			
			//30
			wstr(maple_sega_mic_name,30);

			//60
			wstr(maple_sega_brand,60);

			//2
			w16(0x01AE); 

			//2
			w16(0x01F4);

			return cmd == MDC_DeviceRequest ? MDRS_DeviceStatus : MDRS_DeviceStatusAll;

		/* (A microphone has no condition to ask for: the request is one it
		 * does not know. The answer made here was the controller's with all
		 * but two bytes taken out, six bytes where an answer is whole words.) */

		case MDC_DeviceReset:
			//uhhh do nothing?
			DEBUG_LOG(MAPLE, "maple_microphone::dma MDC_DeviceReset\n");
			return MDRS_DeviceReply;

		case MDCF_MICControl:
		{
			//LOGD("maple_microphone::dma handling MDCF_MICControl %d\n",cmd);
			//MONEY
			u32 function=r32();
			//LOGD("maple_microphone::dma MDCF_MICControl function (1st word) %#010x\n", function);
			//LOGD("maple_microphone::dma MDCF_MICControl words: %d\n", dma_count_in);

			switch(function)
			{
			case MFID_4_Mic:
			{
				//MAGIC HERE
				//http://dcemulation.org/phpBB/viewtopic.php?f=34&t=69600
				// <3 <3 BlueCrab <3 <3
				/*
				2nd word               What it does:
				0x0000??03          Sets the amplifier gain, ?? can be from 00 to 1F
									0x0f = default
				0x00008002          Enables recording
				0x00000001          Returns sampled data while recording is enabled
									While not enabled, returns status of the mic.
				0x00000002          Disables recording
				 *
				 */
				u32 secondword=r32();
				//LOGD("maple_microphone::dma MDCF_MICControl subcommand (2nd word) %#010x\n", subcommand);

				u32 subcommand = secondword & 0xFF; //just get last byte for now, deal with params later

				//LOGD("maple_microphone::dma MDCF_MICControl (3rd word) %#010x\n", r32());
				//LOGD("maple_microphone::dma MDCF_MICControl (4th word) %#010x\n", r32());
				switch(subcommand)
				{
				case 0x01:
				{
					//printf("maple_microphone::dma MDCF_MICControl someone wants some data! (2nd word) %#010x\n", secondword);

					w32(MFID_4_Mic);

					//from what i can tell this is up to spec but results in transmit again
					//w32(secondword);

					//32 bit header
					w8(0x04);//status (just the bit for recording)
					w8(0x0f);//gain (default)
					w8(0);//exp ?
					if(get_mic_data(micdata)){
						w8(240);//ct (240 samples)
						wptr(micdata, SIZE_OF_MIC_DATA);
					}else
					{
						w8(0);
					}

					return MDRS_DataTransfer;
				}
				case 0x02:
					DEBUG_LOG(MAPLE, "maple_microphone::dma MDCF_MICControl toggle recording %#010x\n", secondword);
					return MDRS_DeviceReply;
				case 0x03:
					DEBUG_LOG(MAPLE, "maple_microphone::dma MDCF_MICControl set gain %#010x\n", secondword);
					return MDRS_DeviceReply;
				case MDRE_TransmitAgain:
					WARN_LOG(MAPLE, "maple_microphone::dma MDCF_MICControl MDRE_TransmitAgain\n");
					//apparently this doesnt matter
					//wptr(micdata, SIZE_OF_MIC_DATA);
					return MDRS_DeviceReply;//MDRS_DataTransfer;
				default:
					INFO_LOG(MAPLE, "maple_microphone::dma UNHANDLED secondword %#010x\n", secondword);
               return MDRE_UnknownFunction;
				}
			}
			default:
				INFO_LOG(MAPLE, "maple_microphone::dma UNHANDLED function %#010x\n", function);
            return MDRE_UnknownFunction;
			}
		}

		case MDC_DeviceKill:
			return MDRS_DeviceReply;

		default:
			INFO_LOG(MAPLE, "maple_microphone::dma UNHANDLED MAPLE COMMAND %d\n", cmd);
         return MDRE_UnknownCmd;
		}
	}	
};

struct maple_sega_purupuru : maple_base
{
   u16 AST = 19, AST_ms = 5000;
   u32 VIBSET;

   virtual MapleDeviceType get_device_type()
   {
	  return MDT_PurupuruPack;
   }

   virtual bool maple_serialize(void **data, unsigned int *total_size)
   {
      LIBRETRO_S(AST);
      LIBRETRO_S(AST_ms);
      LIBRETRO_S(VIBSET);
      return true ;
   }
   virtual bool maple_unserialize(void **data, unsigned int *total_size)
   {
      LIBRETRO_US(AST);
      LIBRETRO_US(AST_ms);
      LIBRETRO_US(VIBSET);
      return true ;
   }
   virtual u32 dma(u32 cmd)
   {
      switch (cmd)
      {
         case MDC_DeviceRequest:
         case MDC_AllStatusReq:
            //caps
            //4
            w32(MFID_8_Vibration);

            //struct data
            //3*4
            w32(0x00000101);
            w32(0);
            w32(0);

            //1	area code
            w8(0xFF);

            //1	direction
            w8(0);

            //30
            wstr(maple_sega_purupuru_name, 30);

            //60
            wstr(maple_sega_brand, 60);

            //2
            w16(0x00C8);

            //2
            w16(0x0640);

            return cmd == MDC_DeviceRequest ? MDRS_DeviceStatus : MDRS_DeviceStatusAll;

            //get last vibration
         case MDCF_GetCondition:

            w32(MFID_8_Vibration);

            w32(VIBSET);

            return MDRS_DataTransfer;

         case MDCF_GetMediaInfo:

            w32(MFID_8_Vibration);

            // PuruPuru vib specs
            w32(0x3B07E010);

            return MDRS_DataTransfer;

         case MDCF_BlockRead:

            w32(MFID_8_Vibration);
            w32(0);

            w16(2);
            w16(AST);

            return MDRS_DataTransfer;

         case MDCF_BlockWrite:

            //Auto-stop time
            // (if the frame is long enough to have one)
            if (dma_count_in >= 12)
            {
               AST = dma_buffer_in[10];
               AST_ms = AST * 250 + 250;
            }

            return MDRS_DeviceReply;

         case MDCF_SetCondition:

            // (a frame too short to say how is not acted on)
            if (dma_count_in >= 8)
            {
               VIBSET = *(u32*)&dma_buffer_in[4];
               //Do the rumble thing!
               mcfg_set_vibration(config, VIBSET, AST_ms);
            }

            return MDRS_DeviceReply;

		case MDC_DeviceReset:
			return MDRS_DeviceReply;

		case MDC_DeviceKill:
			return MDRS_DeviceReply;

         default:
			INFO_LOG(MAPLE, "UNKOWN MAPLE COMMAND %d\n", cmd);
            return MDRE_UnknownCmd;
      }
   }
};

char EEPROM[0x100];
bool EEPROM_loaded = false;


/* The EEPROM's first record, the BIOS's settings: bytes 2 to 17, their
 * CRC in bytes 0 and 1, and all of it again in bytes 18 to 35. Byte 8 is
 * the cabinet: bit 0 the coin chutes (individual), bits 4-5 the players
 * less one. */
static u16 naomi_eeprom_crc(const u8 *buf, int size)
{
	u32 n = 0xdebdeb00;

	for (int i = 0; i <= size; i++)
	{
		// (and a zero byte after the last)
		n = (n & 0xffffff00) + (i < size ? buf[i] : 0);
		for (int c = 0; c < 8; c++)
			n = (n & 0x80000000) ? (n << 1) + 0x10210000 : n << 1;
	}
	return n >> 16;
}

static void naomi_eeprom_bios_set(u32 offset, u8 value)
{
	u8 *eeprom = (u8 *)EEPROM;
	eeprom[offset] = value;
	eeprom[offset + 18] = value;
	const u16 crc = naomi_eeprom_crc(eeprom + 2, 16);
	eeprom[0] = eeprom[18] = (u8)crc;
	eeprom[1] = eeprom[19] = (u8)(crc >> 8);
}

/* A cabinet of these games is set for four players when the EEPROM has
 * not been set up for the game (its file is not there, or is another
 * game's): the BIOS's settings are then what the BIOS would make from the
 * game's header, with the four-player cabinet. One that has been is left
 * as it is, unless its number of players is one the header says the game
 * cannot have. */
static void naomi_eeprom_dual_cabinet(void)
{
	u8 header[0x500];
	const u8 *eeprom = (const u8 *)EEPROM;

	if (!jvs_dual_reversed_game() || !naomi_cart_GetBootHeader(header, sizeof(header)))
		return;
	const u8 *game_id = &header[0x134];
	const u8 *coin = &header[0x1e0];		// the first region's coin settings
	const u8 cabinet = header[0x429];		// the numbers of players the game can have, a bit each
	const u8 vertical = header[0x42b];

	if (memcmp(game_id, eeprom + 3, 4))
	{
		for (int i = 0; i < 4; i++)
			naomi_eeprom_bios_set(3 + i, game_id[i]);
		naomi_eeprom_bios_set(7, 9);
		naomi_eeprom_bios_set(8, cabinet == 0 || (cabinet & 8) ? 0x30
				: (cabinet & 4) ? 0x20 : (cabinet & 2) ? 0x10 : 0);
		if (coin[0] == 1)
		{
			// the game's own
			naomi_eeprom_bios_set(2, (coin[1] & 1) | 0x10);
			if (coin[2] == 1)
				naomi_eeprom_bios_set(8, eeprom[8] | 1);
			naomi_eeprom_bios_set(9, coin[3] - 1);
			naomi_eeprom_bios_set(10, std::max(coin[6], (u8)1));
			naomi_eeprom_bios_set(11, std::max(coin[4], (u8)1));
			naomi_eeprom_bios_set(12, std::max(coin[5], (u8)1));
			naomi_eeprom_bios_set(13, coin[7]);
			for (int i = 0; i < 4; i++)
				naomi_eeprom_bios_set(14 + i, coin[8 + i * 2] | (coin[9 + i * 2] << 4));
		}
		else
		{
			// the BIOS's
			naomi_eeprom_bios_set(2, (vertical & 2) ? 0x11 : 0x10);
			naomi_eeprom_bios_set(9, 0);
			naomi_eeprom_bios_set(10, 1);
			naomi_eeprom_bios_set(11, 1);
			naomi_eeprom_bios_set(12, 1);
			naomi_eeprom_bios_set(13, 0);
			for (int i = 0; i < 4; i++)
				naomi_eeprom_bios_set(14 + i, 0x11);
		}
		NOTICE_LOG(MAPLE, "EEPROM: set up for game %s, %d-player cabinet\n", naomi_game_id, (eeprom[8] >> 4) + 1);
	}
	if (cabinet != 0 && cabinet < 0x10 && !(cabinet & (1 << (eeprom[8] >> 4))))
	{
		const u8 players = (cabinet & 8) ? 0x30 : (cabinet & 4) ? 0x20 : (cabinet & 2) ? 0x10 : 0;
		NOTICE_LOG(MAPLE, "EEPROM: %d-player cabinet\n", (players >> 4) + 1);
		naomi_eeprom_bios_set(8, players | (eeprom[8] & 1));
	}
}

u8 kb_shift; 		// shift keys pressed (bitmask)
u8 kb_led; 			// leds currently lit
u8 kb_key[6]={0};	// normal keys pressed

struct maple_keyboard : maple_base
{
	virtual MapleDeviceType get_device_type()
	{
		return MDT_Keyboard;
	}

	virtual u32 dma(u32 cmd)
	{
		switch (cmd)
		{
		case MDC_DeviceRequest:
		case MDC_AllStatusReq:
			//caps
			//4
         w32(MFID_6_Keyboard);
 			//struct data
			//3*4
         w32(0x80000502);	// US, 104 keys
			w32(0);
			w32(0);
			//1	area code
			w8(0xFF);
			//1	direction
			w8(0);
         // Product name (30)
			wstr(maple_sega_kbd_name, 30);

         // License (60)
			wstr(maple_sega_brand, 60);

         // Low-consumption standby current (2)
			w16(0x01AE);

         // Maximum current consumption (2)
			w16(0x01F5);
         
         return cmd == MDC_DeviceRequest ? MDRS_DeviceStatus : MDRS_DeviceStatusAll;
 		case MDCF_GetCondition:
         w32(MFID_6_Keyboard);
			//struct data
			//int8 shift          ; shift keys pressed (bitmask)	//1
			w8(kb_shift);
			//int8 led            ; leds currently lit			//1
			w8(kb_led);
			//int8 key[6]         ; normal keys pressed			//6
         for (int i = 0; i < 6; i++)
			{
				w8(kb_key[i]);
			}
			return MDRS_DataTransfer;

		case MDC_DeviceReset:
			return MDRS_DeviceReply;

		case MDC_DeviceKill:
			return MDRS_DeviceReply;

		default:
			INFO_LOG(MAPLE, "Keyboard: unknown MAPLE COMMAND %d\n", cmd);
		 return MDRE_UnknownCmd;
		}
	}
};

// Mouse buttons
// bit 0: Button C
// bit 1: Right button (B)
// bit 2: Left button (A)
// bit 3: Wheel button
u32 mo_buttons[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
// Relative mouse coordinates [-512:511]
f32 mo_x_delta[4];
f32 mo_y_delta[4];
f32 mo_wheel_delta[4];
// Absolute mouse coordinates
// Range [0:639] [0:479]
// but may be outside this range if the pointer is offscreen or outside the 4:3 window.
f32 mo_x_abs[4];
f32 mo_y_abs[4];

struct maple_mouse : maple_base
{
	virtual MapleDeviceType get_device_type()
	{
		return MDT_Mouse;
	}

	static u16 mo_cvt(f32 delta)
	{
		delta += 0x200;
		if (delta<=0)
			delta=0;
		else if (delta>0x3FF)
			delta=0x3FF;

		return (u16)lroundf(delta);
	}
 	virtual u32 dma(u32 cmd)
	{
		switch (cmd)
		{
		case MDC_DeviceRequest:
		case MDC_AllStatusReq:
			//caps
			//4
			w32(MFID_9_Mouse);
 			//struct data
			//3*4
			w32(0x00070E00);	// Mouse, 3 buttons, 3 axes
			w32(0);
			w32(0);
			//1	area code
			w8(0xFF);
			//1	direction
			w8(0);
			// Product name (30)
			wstr(maple_sega_mouse_name, 30);

 			// License (60)
			wstr(maple_sega_brand, 60);

 			// Low-consumption standby current (2)
			w16(0x0069);
 			// Maximum current consumption (2)
			w16(0x0120);

			return cmd == MDC_DeviceRequest ? MDRS_DeviceStatus : MDRS_DeviceStatusAll;

 		case MDCF_GetCondition:
		    {
		       u32 buttons;
		       f32 delta_x, delta_y, delta_wheel;
			   mcfg_get_mouse(config, &buttons, &delta_x, &delta_y, &delta_wheel);

			   w32(MFID_9_Mouse);
			   //struct data
			   /* int8 buttons, pressed is 0: bit 2 left, bit 1 right, bit 3
			    * the middle one (the wheel's). The three bytes after it are
			    * options, the axes that overflowed and one kept for later,
			    * and are 0. (All four were sent as one word of buttons with
			    * every bit above the three set: every axis always
			    * overflowing. Upstream 9d4711a95: Silent Scope, Dreamkey.) */
			   w8((u8)buttons);
			   w8(0);
			   w8(0);
			   w8(0);
			   //int16 axis1         ; horizontal movement (0-$3FF) (little endian)
			   w16(mo_cvt(delta_x));
			   //int16 axis2         ; vertical movement (0-$3FF) (little endian)
			   w16(mo_cvt(delta_y));
			   //int16 axis3         ; mouse wheel movement (0-$3FF) (little endian)
			   w16(mo_cvt(delta_wheel));
			   //int16 axis4         ; ? movement (0-$3FF) (little endian)
			   w16(mo_cvt(0));
			   //int16 axis5         ; ? movement (0-$3FF) (little endian)
			   w16(mo_cvt(0));
			   //int16 axis6         ; ? movement (0-$3FF) (little endian)
			   w16(mo_cvt(0));
			   //int16 axis7         ; ? movement (0-$3FF) (little endian)
			   w16(mo_cvt(0));
			   //int16 axis8         ; ? movement (0-$3FF) (little endian)
			   w16(mo_cvt(0));
		    }

			return MDRS_DataTransfer;

		case MDC_DeviceReset:
			return MDRS_DeviceReply;

		case MDC_DeviceKill:
			return MDRS_DeviceReply;

 		default:
			INFO_LOG(MAPLE, "Mouse: unknown MAPLE COMMAND %d\n", cmd);
			return MDRE_UnknownCmd;
		}
	}
};

struct maple_lightgun : maple_base
{
	virtual u32 transform_kcode(u32 kcode) {
		return kcode | 0xFF01;
	}

   virtual MapleDeviceType get_device_type()
   {
	  return MDT_LightGun;
   }

   virtual u32 dma(u32 cmd)
   {
	  switch (cmd)
	  {
	  case MDC_DeviceRequest:
	  case MDC_AllStatusReq:
		 //caps
		 //4
		 w32(MFID_7_LightGun | MFID_0_Input);

		 //struct data
		 //3*4
		 w32(0);				// Light gun
		 w32(0xFE000000);	// Controller
		 w32(0);
		 //1	area code
		 w8(0x01);		// FF: Worldwide, 01: North America
		 //1	direction
		 w8(0);
		 // Product name (30)
		 wstr(maple_sega_lightgun_name, 30);

		 // License (60)
		 wstr(maple_sega_brand, 60);

		 // Low-consumption standby current (2)
		 w16(0x0069);	// 10.5 mA

		 // Maximum current consumption (2)
		 w16(0x0120);	// 28.8 mA

		 return cmd == MDC_DeviceRequest ? MDRS_DeviceStatus : MDRS_DeviceStatusAll;

	  case MDCF_GetCondition:
	  {
		 PlainJoystickState pjs;
		 pjs_init(&pjs);
		 mcfg_get_input(config, &pjs);

		 //caps
		 //4
		 w32(MFID_0_Input);

		 //state data
		 //2 key code
			w16(transform_kcode(pjs.kcode));

		 // the two trigger axes, which a gun has not: at rest. (They read as
		 // fully pulled, and Death Crimson 2 took that for "skip the story".)
		 //2
		 w16(0);

		 //not used
		 //4
		 w32(0x80808080);
	  }

	  return MDRS_DataTransfer;

		case MDC_DeviceReset:
			return MDRS_DeviceReply;

		case MDC_DeviceKill:
			return MDRS_DeviceReply;

	  default:
			INFO_LOG(MAPLE, "Light gun: unknown MAPLE COMMAND %d\n", cmd);
		 return MDRE_UnknownCmd;
	  }
   }

   virtual bool get_lightgun_pos()
   {
	  f32 x, y;
	  mcfg_get_absolute_position(config, &x, &y);
	  read_lightgun_position(x + 0.5f, y + 0.5f);
     return true;
   }
};

struct atomiswave_lightgun : maple_lightgun
{
	virtual u32 transform_kcode(u32 kcode) override {
		return (kcode & AWAVE_TRIGGER_KEY) == 0 ? ~AWAVE_BTN0_KEY : ~0;
	}
};


/*
   Sega JVS I/O boards: jvs_board.c
*/

/* The program a game sends to the I/O board's processor, a piece at a time
 * over many frames: which program it is decides how the board's requests
 * are read (crazy_mode). All zeroes when none is being sent. In a save
 * state from V23: a state loaded while one is on its way goes on with what
 * had been sent by then. */
static u8 jvs_firmware[0x10000];

/* What is on the I/O board bridge's RS-422 port: Derby Owners Club's card
 * reader, through a converter board. Not the bridge's own: the bridge is
 * made anew by a state load. */
static serial_pipe *mie_pipe;

void naomi_mie_set_pipe(serial_pipe *pipe)
{
	mie_pipe = pipe;
}

struct maple_naomi_jamma : maple_sega_controller
{
	const u8 ALL_NODES = 0xff;

	/* (on the bridge's bus, in it: made as it is) */
	jvs_boards boards = {};
	bool crazy_mode = false;
	// The firmware is The House of the Dead 2 prototype's. A state does not
	// have this: on a load it is worked out from the game's name.
	bool hotd2p = false;

	u8 jvs_repeat_request[32][256];
	u8 jvs_receive_buffer[32][258];
	u32 jvs_receive_length[32] = { 0 };
	bool eeprom_dirty = false;
	MapleSaveState save_state;

	maple_naomi_jamma()
	{
		/* The boards are there from the start, as they are on a cabinet:
		 * made only when the game first spoke to them, a state saved
		 * before that was smaller than one saved after, and a frontend
		 * asks the size once. */
		create_io_boards();
	}
	virtual ~maple_naomi_jamma()
	{
		save_state.wait = 0;
		FlushSave();
		if (save_state.wait != 0)
		{
			save_state.wait = 0;
			FlushSave();
		}
		// (the flag is this device's: the writer is done with it first)
		save_writer_drain();
		if (save_state.Failed())
			WARN_LOG(MAPLE, "Cannot save EEPROM to file %s\n", eeprom_file);
		EEPROM_loaded = false;
	}

	/* The EEPROM is read from its file once, when the machine starts
	 * (maple_naomi_eeprom_load()), and is in memory from then on. (It was
	 * read again each time the game asked. What the game writes goes to the
	 * file later, from the writer's thread: asked for before that, the file
	 * was still the old one, and the old one was then what got written.)
	 * The game asking for it reads it here only after a state saved before
	 * it had been read. */
	void load_eeprom()
	{
		if (EEPROM_loaded)
			return;
		EEPROM_loaded = true;
		memset(EEPROM, 0, sizeof(EEPROM));
		RFILE* f = filestream_open(eeprom_file, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if (f)
		{
			filestream_read(f, EEPROM, 0x80);
			filestream_close(f);
			DEBUG_LOG(MAPLE, "Loaded EEPROM from %s\n", eeprom_file);
		}
		else if (naomi_default_eeprom != NULL)
			memcpy(EEPROM, naomi_default_eeprom, 0x80);
		naomi_eeprom_dual_cabinet();
	}

	/* The EEPROM, when the game has written to it, to its file: made
	 * anew each time, by the writer's thread (savewriter.h). Tried again
	 * later if that fails, as a memory card's is. */
	virtual void FlushSave()
	{
		if (const int failed = save_state.Failed())
		{
			if (failed == 1)
				WARN_LOG(MAPLE, "Cannot save EEPROM to file %s. It will be tried again\n", eeprom_file);
			eeprom_dirty = true;
		}
		if (!eeprom_dirty || save_state.Waiting())
			return;
		if (save_writer_put_file(eeprom_file, EEPROM, 0x80, &save_state.failed))
			eeprom_dirty = false;
	}

	void create_io_boards()
	{
	   jvs_boards_create(&boards);
	}

	virtual MapleDeviceType get_device_type()
	{
		return MDT_NaomiJamma;
	}

	u8 sense_line(u32 node_id)
	{
		bool last_node = node_id == boards.count;
		return last_node ? 0x8E : 0x8F;
	}

	void send_jvs_message(u32 node_id, u32 channel, u32 length, u8 *data)
	{
		if (node_id - 1 < boards.count)
		{
			if (boards.cabinet_order)
				jvs_boards_order(&boards);
			u8 temp_buffer[256];
			u32 out_len = jvs_board_message(&boards, node_id - 1, config, data, length, temp_buffer);
			if (out_len > 0)
			{
				u8 *pbuf = &jvs_receive_buffer[channel][jvs_receive_length[channel]];
				if (jvs_receive_length[channel] + out_len + 3 <= sizeof(jvs_receive_buffer[0]))
				{
					if (crazy_mode)
					{
						pbuf[0] = 0x00;		// ? 0: ok, 2: timeout, 3: dest node !=0, 4: checksum failed
						pbuf[1] = out_len;
						memcpy(&pbuf[2], temp_buffer, out_len);
						jvs_receive_length[channel] += out_len + 2;
					}
					else
					{
						pbuf[0] = node_id;
						pbuf[1] = 0x00;		// 0: ok, 2: timeout, 3: dest node !=0, 4: checksum failed
						pbuf[2] = out_len;
						memcpy(&pbuf[3], temp_buffer, out_len);
						jvs_receive_length[channel] += out_len + 3;
					}
				}
			}
		}
	}

	void send_jvs_messages(u32 node_id, u32 channel, bool use_repeat, u32 length, u8 *data, bool repeat_first)
	{
		/* A request is 255 bytes at the most. The four more are zeroes put
		 * after it: a request that stops short of what its code needs is read
		 * by a board that far past its end, and no further. */
		u8 temp_buffer[256 + 4];
		if (data)
		{
			memcpy(temp_buffer, data, length);
		}
		if (node_id == ALL_NODES)
		{
			memset(temp_buffer + length, 0, 4);
			for (u32 i = 0; i < boards.count; i++)
				send_jvs_message(i + 1, channel, length, temp_buffer);
		}
		else if (node_id >= 1 && node_id <= 32)
		{
			u32 repeat_len = jvs_repeat_request[node_id - 1][0];
			// (a stored request is left out if it and this one together are longer than a request can be)
			if (use_repeat && repeat_len > 0 && length + repeat_len <= 256)
			{
				if (repeat_first)
				{
				   memmove(temp_buffer + repeat_len, temp_buffer, length);
				   memcpy(temp_buffer, &jvs_repeat_request[node_id - 1][1], repeat_len);
				}
				else
				{
				   memcpy(temp_buffer + length, &jvs_repeat_request[node_id - 1][1], repeat_len);
				}
				length += repeat_len;
			}
			memset(temp_buffer + length, 0, 4);
			send_jvs_message(node_id, channel, length, temp_buffer);
		}
	}

	void receive_jvs_messages(u32 channel)
	{
	   if (dma_count_in == 0)
	   {
		  w8(MDRS_JVSReply);
		  w8(0);
		  w8(0x20);
		  w8(0x00);

		  return;
	   }
	   u32 dword_length = (jvs_receive_length[channel] + 0x10 + 3 - 1) / 4 + 1;

	   w8(MDRS_JVSReply);
	   w8(0x00);
	   w8(0x20);
	   if (jvs_receive_length[channel] == 0)
	   {
		  w8(0x05);
		  w8(0x32);
	   }
	   else
	   {
		  w8(dword_length);
		  w8(0x16);
	   }
	   w8(0xff);
	   w8(0xff);
	   w8(0xff);
	   w32(0xffffff00);
	   w32(0);
	   w32(0);

	   if (jvs_receive_length[channel] == 0)
	   {
		  w32(0);
		  return;
	   }

	   w8(0);
	   w8(channel);
	   if (crazy_mode)
		  w8(0x8E);
	   else
		  w8(sense_line(jvs_receive_buffer[channel][0]));	// bit 0 is sense line level. If set during F1 <n>, more I/O boards need addressing

	   memcpy(dma_buffer_out, jvs_receive_buffer[channel], jvs_receive_length[channel]);
	   /* The reply is made up to whole words, and what is past the message
	    * is zeroes: it was whatever the buffer had there (upstream
	    * 58f05f74c, an SH4 crash while a NAOMI started). */
	   memset(dma_buffer_out + jvs_receive_length[channel], 0,
			 dword_length * 4 - 0x10 - 3 - jvs_receive_length[channel]);
	   dma_buffer_out += dword_length * 4 - 0x10 - 3;
	   *dma_count_out += dword_length * 4 - 0x10 - 3;
	   jvs_receive_length[channel] = 0;
	}

	void handle_86_subcommand()
	{
	   if (dma_count_in == 0)
	   {
		  w8(MDRS_JVSReply);
		  w8(0);
		  w8(0x20);
		  w8(0x00);
		  return;
	   }
	   u32 subcode = dma_buffer_in[0];

		// CT fw uses 13 as a 17, and 17 as 13 and also uses 19
		if (crazy_mode)
		{
			switch (subcode)
			{
			case 0x13:
				subcode = 0x17;
				break;
			case 0x17:
				subcode = 0x13;
				break;
			}
		}
		u8 node_id = 0;
		u8 *cmd = NULL;
		u32 len = 0;
		u8 channel = 0;
		if (dma_count_in >= 3)
		{
			// every request but "store" has its channel, node and length further on, if it is long enough
			if (subcode != 0x13 && dma_count_in >= 8)
			{
				node_id = dma_buffer_in[6];
				len = dma_buffer_in[7];
				cmd = &dma_buffer_in[8];
				channel = dma_buffer_in[5] & 0x1f;
			}
			else
			{
				node_id = dma_buffer_in[1];
				len = dma_buffer_in[2];
				cmd = &dma_buffer_in[3];
			}
			// (the length is the game's: no more than the frame has after it)
			if (len > (u32)(dma_buffer_in + dma_count_in - cmd))
				len = dma_buffer_in + dma_count_in - cmd;
		}

		switch (subcode)
		{
			case 0x13:	// Store repeated request
				if (len > 0 && node_id > 0 && node_id <= 0x1f)
				{
					INFO_LOG(MAPLE, "JVS node %d: Storing %d cmd bytes\n", node_id, len);
					jvs_repeat_request[node_id - 1][0] = len;
					memcpy(&jvs_repeat_request[node_id - 1][1], cmd, len);
				}
				w8(MDRS_JVSReply);
				w8(0);
				w8(0x20);
				w8(0x01);
				w8(dma_buffer_in[0] + 1);	// subcommand + 1
				w8(0);
				w8(len + 1);
				w8(0);
				break;

			case 0x15:	// Receive JVS data
				// (there are 32 channels)
				receive_jvs_messages(dma_buffer_in[1] & 0x1f);
				if (hotd2p)
				{
					// this firmware sends its next request along with it
					send_jvs_messages(node_id, channel, true, len, cmd, false);
					w8(MDRS_JVSReply);
					w8(0);
					w8(0x20);
					w8(0x01);
					w8(0x18);	// always
					w8(channel);
					w8(sense_line(node_id));
					w8(0);
				}
				break;

			case 0x17:	// Transmit without repeat
				jvs_receive_length[channel] = 0;
				send_jvs_messages(node_id, channel, false, len, cmd, false);
				w8(MDRS_JVSReply);
				w8(0);
				w8(0x20);
				w8(0x01);
				w8(0x18);	// always
				w8(channel);
				w8(0x8E);	//sense_line(node_id));
				w8(0);
				break;

			case 0x19:	// Transmit with repeat
				jvs_receive_length[channel] = 0;
				send_jvs_messages(node_id, channel, true, len, cmd, true);
				w8(MDRS_JVSReply);
				w8(0);
				w8(0x20);
				w8(0x01);
				w8(0x18);	// always
				w8(channel);
				w8(sense_line(node_id));
				w8(0);
				break;

			case 0x21:	// Transmit with repeat
			   jvs_receive_length[channel] = 0;
			   send_jvs_messages(node_id, channel, true, len, cmd, false);
			   w8(MDRS_JVSReply);
			   w8(0);
			   w8(0x20);
			   w8(0x01);
			   w8(0x18);	// always
			   w8(channel);
			   w8(sense_line(node_id));
			   w8(0);
			   break;

			case 0x35:	// Receive then transmit with repeat (15 then 27)
				receive_jvs_messages(channel);
				// FALLTHROUGH

			case 0x27:	// Transmit with repeat
				{
					jvs_receive_length[channel] = 0;

					/* How many requests, and how long each is, are the game's
					 * to say: no more than the frame has is read. */
					u32 cmd_count = dma_count_in >= 8 ? dma_buffer_in[6] : 0;
					u32 idx = 7;
					for (int i = 0; i < cmd_count && idx + 2 <= dma_count_in; i++)
					{
						node_id = dma_buffer_in[idx];
						len = dma_buffer_in[idx + 1];
						cmd = &dma_buffer_in[idx + 2];
						if (len > dma_count_in - idx - 2)
							len = dma_count_in - idx - 2;
						idx += len + 2;

						send_jvs_messages(node_id, channel, true, len, cmd, false);
					}

					w8(MDRS_JVSReply);
					w8(0);
					w8(0x20);
					w8(0x01);
					w8(0x26);
					w8(channel);
					w8(sense_line(node_id));
					w8(0);
				}
				break;

			case 0x33:	// Receive then transmit with repeat (15 then 21)
				receive_jvs_messages(channel);
				send_jvs_messages(node_id, channel, true, len, cmd, false);
				w8(MDRS_JVSReply);
				w8(0);
				w8(0x20);
				w8(0x01);
				w8(0x18);	// always
				w8(channel);
				w8(sense_line(node_id));
				w8(0);
				break;

			case 0x0B:	//EEPROM write
			{
				int address = dma_buffer_in[1];
				int size = dma_buffer_in[2];
				DEBUG_LOG(MAPLE, "EEprom write %08X %08X\n", address, size);
				//printState(Command,buffer_in,buffer_in_len);
				load_eeprom();
				// (the chip has 0x80 bytes; the game's address and size went unchecked)
				address &= 0x7f;
				if (size > 0x80 - address)
					size = 0x80 - address;
				// (nor more than the frame has)
				if (size > (int)dma_count_in - 4)
					size = (int)dma_count_in - 4;
				memcpy(EEPROM + address, dma_buffer_in + 4, size);
				eeprom_dirty = true;

				w8(MDRS_JVSReply);
				w8(0x00);
				w8(0x20);
				w8(0x01);
				memcpy(dma_buffer_out, EEPROM, 4);
				dma_buffer_out += 4;
				*dma_count_out += 4;
			}
			break;

			case 0x3:	//EEPROM read
			{
				load_eeprom();

				//printf("EEprom READ\n");
				// (0x80 bytes from an address below 0x80 are inside the array, which has 0x100)
				int address = dma_buffer_in[1] & 0x7f;
				//printState(Command,buffer_in,buffer_in_len);
				w8(MDRS_JVSReply);
				w8(0x00);
				w8(0x20);
				w8(0x20);
				memcpy(dma_buffer_out, EEPROM + address, 0x80);
				dma_buffer_out += 0x80;
				*dma_count_out += 0x80;
			}
			break;

			case 0x31:	// DIP switches
			{
				w8(MDRS_JVSReply);
				w8(0x00);
				w8(0x20);
				w8(0x05);

				w8(0x32);
				w8(0xff);		// in(0)
				w8(0xff);		// in(1)
				w8(0xff);		// in(2)

				w8(0x00);
				w8(0xff);		// in(4)
				{
					// in(5) bit0: 1=VGA, 0=NTSCi
					u8 dip = enable_naomi_15khz_dipswitch ? 0x00 : 0xff;
					/* Switches 2 to 4 say the region to the multi-region
					 * BIOS: Korea is 2 and 3 on, 4 off (other BIOSes have their own). */
					if (naomi_bios_multiregion)
						dip = (dip & ~0x0e) | 0x08;
					w8(dip);
				}
				w8(0xff);		// in(6)

				w32(0x00);
				w32(0x00);
				w32(0x00);
			}
			break;

			//case 0x3:
			//	break;

			case 0x1:
				w8(MDRS_JVSReply);
				w8(0x00);
				w8(0x20);
				w8(0x01);

				w8(0x2);
				w8(0x0);
				w8(0x0);
				w8(0x0);
				break;

			/* The RS-422 port, with a device on it (it is not answered
			 * at all then). Without one the bridge says it knows none of
			 * these, as it always has here. */
			case 0x41: // reset?
				if (mie_pipe == NULL)
					goto unknown;
				DEBUG_LOG(MAPLE, "JVS: RS422 reset\n");
				while (mie_pipe->ops->available(mie_pipe) > 0)
					mie_pipe->ops->read(mie_pipe);
				break;

			case 0x47: // send data
				if (mie_pipe == NULL)
					goto unknown;
				DEBUG_LOG(MAPLE, "JVS: RS422 send %02x\n", dma_buffer_in[4]);
				if (dma_count_in >= 5)
					mie_pipe->ops->write(mie_pipe, dma_buffer_in[4]);
				break;

			case 0x4d: // receive data
				{
					if (mie_pipe == NULL)
						goto unknown;
					const int avail = std::min(mie_pipe->ops->available(mie_pipe), 0xfe);
					DEBUG_LOG(MAPLE, "JVS: RS422 receive %d bytes\n", avail);
					w8(MDRS_JVSReply);
					w8(0x00);
					w8(0x20);
					w8(1 + (avail + 3) / 4);
					w8(0);
					w8(0);
					w8(0);
					w8(avail == 0 ? 0xff : avail); // 0xff => no data, else byte count

					for (int i = 0; i < ((avail + 3) / 4) * 4; i++)
						w8(i >= avail ? 0 : mie_pipe->ops->read(mie_pipe));
					break;
				}

			case 0x49: // I?
			case 0x4b: // K?
			case 0x4f: // O?
				if (mie_pipe == NULL)
					goto unknown;
				break;

			default:
			unknown:
				INFO_LOG(MAPLE, "JVS: Unknown 0x86 sub-command %x\n", subcode);
				w8(MDRE_UnknownCmd);
				w8(0x00);
				w8(0x20);
				w8(0x00);
				break;
		}
	}

	virtual u32 RawDma(u32* buffer_in, u32 buffer_in_len, u32* buffer_out)
	{
#ifdef DUMP_JVS
	   printf("JVS IN: ");
	   u8 *p = (u8*)buffer_in;
	   for (int i = 0; i < buffer_in_len; i++) printf("%02x ", *p++);
	   printf("\n");
#endif
	   create_io_boards();
	   // the card reader and hopper buttons, read as the board's inputs are
	   naomi_periph_poll();

		u32 out_len = 0;
		dma_buffer_out = (u8 *)buffer_out;
		dma_count_out = &out_len;

		dma_buffer_in = (u8 *)buffer_in + 4;
		dma_count_in = buffer_in_len - 4;

		u32 cmd = *(u8*)buffer_in;
		switch (cmd)
		{
			case MDC_JVSSelfTest:
				w8(MDRS_JVSSelfTestReply);
				w8(0x00);
				w8(0x20);
				w8(0x01);
				w8(0x00);
				break;

			case MDC_JVSCommand:
				handle_86_subcommand();
				break;

			case MDC_JVSUploadFirmware:
			{
				u8 *ram = jvs_firmware;

				if (dma_count_in < 4)
				{
					// (not long enough to say where its bytes go)
					w8(MDRE_UnknownCmd);
					w8(0x00);
					w8(0x00);
					w8(0x00);
					break;
				}

				if (dma_buffer_in[1] == 0xff)
				{
					u32 hash = xxh32(ram, 0x10000, 0);
					LOGJVS("JVS Firmware hash %08x\n", hash);
					hotd2p = hash == 0xa6784e26;
					if (hash == 0xa7c50459			// CT
						  || hash == 0xae841e36	// HOTD2
						  || hotd2p)
						crazy_mode = true;
					else
						crazy_mode = false;
#ifdef DUMP_JVS_FW
					RFILE *fw_dump;
					char filename[128];
					for (int i = 0; ; i++)
					{
						sprintf(filename, "z80_fw_%d.bin", i);
						fw_dump = filestream_open(filename, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
						if (fw_dump == NULL)
						{
							fw_dump = filestream_open(filename, RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
							INFO_LOG(MAPLE, "Saving JVS firmware to %s\n", filename);
							break;
						}
					}
					if (fw_dump)
					{
						filestream_write(fw_dump, ram, 0x10000);
						filestream_close(fw_dump);
					}
#endif
					// (empty for the next one)
					memset(ram, 0, 0x10000);
					for (int i = 0; i < 32; i++)
						jvs_repeat_request[i][0] = 0;

					// (the reply was never written: its code was returned as the length of one)
					w8(MDRS_DeviceReply);
					w8(0x00);
					w8(0x20);
					w8(0x00);
					break;
				}
				int xfer_bytes;
				if (dma_buffer_in[0] == 0xff)
					xfer_bytes = 0x1C;
				else
					xfer_bytes = 0x18;
				u16 addr = (dma_buffer_in[2] << 8) + dma_buffer_in[3];
				// (where the bytes go is the game's to say: what is past the end of the 64 KB is left out)
				if (xfer_bytes > 0x10000 - addr)
					xfer_bytes = 0x10000 - addr;
				// (nor what is past the end of the frame, which may be the end of RAM)
				if (xfer_bytes > (int)dma_count_in - 4)
					xfer_bytes = (int)dma_count_in - 4;
				memcpy(ram + addr, &dma_buffer_in[4], xfer_bytes);
				u8 sum = 0;
				for (u32 i = 0; i < 0x1C && i < dma_count_in; i++)
					sum += dma_buffer_in[i];

				w8(0x80);	// or 0x81 on bootrom?
				w8(0);
				w8(0x20);
				w8(0x01);
				w8(sum);
				w8(0);
				w8(0);
				w8(0);

				w8(MDRS_DeviceReply);
				w8(0x00);
				w8(0x20);
				w8(0x00);
			}
			break;

		case MDC_JVSGetId:
			{
				const char ID1[] = "315-6149    COPYRIGHT SEGA E";
				const char ID2[] = "NTERPRISES CO,LTD.  1998    ";
				w8(0x83);
				w8(0x00);
				w8(0x20);
				w8(0x07);
				wstr(ID1, 28);

				w8(0x83);
				w8(0x00);
				w8(0x20);
				w8(0x05);
				wstr(ID2, 28);
			}
			break;

		case MDC_DeviceRequest:
			w8(MDRS_DeviceStatus);
			w8(0x00);
			w8(0x20);
			w8(0x00);
			break;

		case MDC_AllStatusReq:
			w8(MDRS_DeviceStatusAll);
			w8(0x00);
			w8(0x20);
			w8(0x00);
			// (it went on into the next answer, and both were sent)
			break;

		case MDC_DeviceReset:
		case MDC_DeviceKill:
			w8(MDRS_DeviceReply);
			w8(0x00);
			w8(0x20);
			w8(0x00);
			break;

		case MDCF_GetCondition:
			w8(MDRE_UnknownCmd);
			w8(0x00);
			w8(0x00);
			w8(0x00);

			break;

		default:
			INFO_LOG(MAPLE, "Unknown Maple command %x\n", cmd);
			w8(MDRE_UnknownCmd);
			w8(0x00);
			w8(0x00);
			w8(0x00);

			break;
		}

#ifdef DUMP_JVS
		printf("JVS OUT: ");
		p = (u8 *)buffer_out;
		for (int i = 0; i < out_len; i++) printf("%02x ", p[i]);
		printf("\n");
#endif

		return out_len;
	}

	virtual bool maple_serialize(void **data, unsigned int *total_size)
	{
	   LIBRETRO_S(crazy_mode);
	   LIBRETRO_S(jvs_repeat_request);
	   LIBRETRO_S(jvs_receive_length);
	   LIBRETRO_S(jvs_receive_buffer);
	   size_t board_count = boards.count;
	   LIBRETRO_S(board_count);
	   for (u32 i = 0; i < boards.count; i++)
		  jvs_board_serialize(&boards.board[i], data, total_size);

	   return true ;
	}

	virtual bool maple_unserialize(void **data, unsigned int *total_size)
	{
	   LIBRETRO_US(crazy_mode);
	   LIBRETRO_US(jvs_repeat_request);
	   LIBRETRO_US(jvs_receive_length);
	   LIBRETRO_US(jvs_receive_buffer);
	   // (a state cannot have more waiting on a channel than a channel holds)
	   for (int i = 0; i < 32; i++)
		  if (jvs_receive_length[i] > sizeof(jvs_receive_buffer[0]))
			 jvs_receive_length[i] = 0;
	   // (the prototype's own firmware is one of those that set crazy_mode; the BIOS's, before it, is not)
	   hotd2p = crazy_mode && !strcmp(naomi_game_id, "hotd2p");
	   create_io_boards();
	   /* How many boards there are is written as the build's own size_t:
	    * eight bytes by a 64-bit build, four by a 32-bit one. The four
	    * more are zeroes. Where a 32-bit build's state goes straight on,
	    * the next byte is the first board's address on the bus or, with no
	    * board, the device on the next port: never 0. So a state of either
	    * build is read by both. (Each took the other's for its own: the
	    * boards and every device after them were read four bytes out.) */
	   u32 board_count = 0;
	   u32 count_high = 0;
	   LIBRETRO_US(board_count);
	   if (LIBRETRO_US(count_high) && count_high != 0)
	   {
		  *data = (u8 *)*data - sizeof(count_high);
		  *total_size -= sizeof(count_high);
	   }
	   /* The boards are this cabinet's, how many the state has is the
	    * state's to say: one it has and the cabinet has not is read past
	    * (what a board keeps: jvs_board_serialize()), and there
	    * are 31 addresses on the bus. */
	   for (u32 i = 0; i < board_count && i < 31; i++)
	   {
		  if (i < boards.count)
			 jvs_board_unserialize(&boards.board[i], data, total_size);
		  else
		  {
			 u8 skipped[sizeof(u8) + sizeof(bool)];
			 LIBRETRO_US(skipped);
		  }
	   }

	   return true;
	}
};

/* The I/O board's program on its way, for naomi_serialize_v23(). */
void naomi_jvs_serialize_v23(void **data, unsigned int *total_size)
{
	LIBRETRO_SA(jvs_firmware, sizeof(jvs_firmware));
}

bool naomi_jvs_unserialize_v23(void **data, unsigned int *total_size)
{
	return LIBRETRO_USA(jvs_firmware, sizeof(jvs_firmware));
}

/* A reset: a program that was on its way to the board is sent again from
 * its start, and what had come of it is not to be taken for part of it. */
void maple_jvs_firmware_reset(void)
{
	memset(jvs_firmware, 0, sizeof(jvs_firmware));
}

// The NAOMI's I/O board device, if the machine has one: it is the first port's, and its boards are the ones in the state
static maple_naomi_jamma *jvs_state_device()
{
   maple_device *dev = MapleDevices[0][5];

   if (dev != NULL && dev->get_device_type() == MDT_NaomiJamma)
	  return (maple_naomi_jamma *)dev;
   return NULL;
}

/* The EEPROM read from its file as the machine starts, before a state can
 * be saved: a state has it then, and not word that it is yet to be read.
 * Read only when the game first asked, a state saved before that was loaded
 * into a machine whose file the game had written to since - which run-ahead
 * does every frame - and the game found what it had not yet written. */
void maple_naomi_eeprom_load(void)
{
   maple_naomi_jamma *jamma = jvs_state_device();

   if (jamma != NULL)
	  jamma->load_eeprom();
}

jvs_boards *maple_jvs_boards(void)
{
   maple_naomi_jamma *jamma = jvs_state_device();

   return jamma != NULL ? &jamma->boards : NULL;
}

/*
	The IC card reader/writers of Virtua Fighter 4 and Mushiking: a
	838-14245-92 maple to RS232 converter wired to a 838-14243 RFID
	reader/writer (apparently a Saxa HW210), one on the second and one on
	the third bus, for the first and second player.

	The cards are the players': what is on them, whether one is in and
	whether the game has locked it in are kept here and not in the
	devices, which a state load makes anew. Each player's card is in a
	file in the save directory, read when the game is started.
*/
struct RfidCard
{
	u8 data[128];
	u8 d4Seen;
	u8 inserted;
	u8 locked;
};
static RfidCard rfid_card[2];
static u8 rfid_file[2][128];
static bool rfid_game;
static retro_atomic_int_t rfid_save_failed;

static const char *rfid_file_suffix(int player)
{
	return player == 0 ? "-p1.card" : "-p2.card";
}

static u8 rfid_random(u32& seed)
{
	seed = seed * 1103515245u + 12345u;
	return seed >> 16;
}

/* A player's card from its file, or a new one: a Mushiking card with its
 * serial number and the key made from it, or a blank Virtua Fighter 4
 * card with the random bytes the game makes its card number of, from
 * @seed (not rand(), which anything else in the process can move on). */
static void rfid_load_card(int player, u32& seed)
{
	u8 *card = rfid_file[player];
	if (periph_load_file(rfid_file_suffix(player), card, sizeof(rfid_file[0])) != 0)
		return;
	if (!strncmp(naomi_game_id, "MKG TKOB", 8))
	{
		static const u8 MUSHIKING_CHIP_DATA[128] = {
			0x12, 0x34, 0x56, 0x78, // Serial No.0
			0x31, 0x00, 0x86, 0x07, // Serial No.1
			0x00, 0x00, 0x00, 0x00, // Key
			0x04, 0xf6, 0x00, 0xAA, // Extend  Extend  Access  Mode
			0x23, 0xFF, 0xFF, 0xFF, // Counter4  Counter3  Counter2  Counter1
			0x00, 0x00, 0x00, 0x00, // User Data (first set date: day bits 0-4, month bits 5-8, year bits 9-... + 2000)
			0x00, 0x00, 0x00, 0x00, // User Data
			0x00, 0x00, 0x00, 0x00, // User Data
			0x00, 0x00, 0x00, 0x00, // User Data
			0x00, 0x00, 0x00, 0x00, // User Data
			0x00, 0x00, 0x00, 0x00, // User Data
			0x23, 0xFF, 0xFF, 0xFF, // User Data (max counters)
		};
		memcpy(card, MUSHIKING_CHIP_DATA, sizeof(MUSHIKING_CHIP_DATA));
		for (int i = 0; i < 8; i++)
			card[i] = rfid_random(seed);
		u32 mask = 0;
		if (!strcmp(naomi_game_id, "MKG TKOB 2 JPN VER2.001-")		// mushik2e
				|| !strcmp(naomi_game_id, "MKG TKOB 4 JPN VER2.000-"))	// mushik4e
			mask = 0x40;
		card[4] &= ~0xc0;
		card[4] |= mask;

		u32 serial1 = (card[4] << 24) | (card[5] << 16) | (card[6] << 8) | card[7];
		u32 key = ~serial1;
		key = ((key >> 4) & 0x0f0f0f0f)
			  | ((key << 4) & 0xf0f0f0f0);
		card[8] = key >> 24;
		card[9] = key >> 16;
		card[10] = key >> 8;
		card[11] = key;
	}
	else
	{
		memset(card, 0, sizeof(rfid_file[0]));
		card[0] = 0x10;
		card[12] = 4;
		card[13] = 0x6c;
		card[127] = 0xff;
		card[2] = rfid_random(seed);
		card[4] = rfid_random(seed);
		card[5] = rfid_random(seed);
		card[6] = rfid_random(seed);
		card[7] = rfid_random(seed);
	}
	INFO_LOG(NAOMI, "Card P%d initialized\n", player + 1);
}

static void rfid_save_card(int player)
{
	memcpy(rfid_file[player], rfid_card[player].data, sizeof(rfid_file[0]));
	periph_save_file(rfid_file_suffix(player), rfid_file[player], sizeof(rfid_file[0]), &rfid_save_failed);
}

/* The game is one with the readers (or is not, with @game false): the
 * cards read from their files, and put in at once with @insert. */
void rfid_init(bool game, bool insert)
{
	rfid_game = game;
	memset(rfid_card, 0, sizeof(rfid_card));
	if (!game)
		return;
	u32 seed = (u32)time(0) * 2654435761u;
	for (int i = 0; i < 2; i++)
	{
		rfid_load_card(i, seed);
		if (insert)
			insertRfidCard(i);
	}
}

// The card is put in, or taken out if it is in and not locked in
void insertRfidCard(int playerNum)
{
	if (!rfid_game || playerNum < 0 || playerNum > 1)
		return;
	RfidCard& card = rfid_card[playerNum];
	if (!card.inserted)
	{
		card.inserted = 1;
		memcpy(card.data, rfid_file[playerNum], sizeof(card.data));
	}
	else if (!card.locked)
	{
		card.inserted = 0;
		memset(card.data, 0, sizeof(card.data));
	}
}

const u8 *getRfidCardData(int playerNum)
{
	if (!rfid_game || playerNum < 0 || playerNum > 1)
		return NULL;
	return rfid_card[playerNum].data;
}

/* The cards in a save state (V24): what is on each, whether it is in and
 * locked, and whether the game asked for its data. 262 bytes. */
void rfid_serialize_v24(void **data, unsigned int *total_size)
{
	if (!rfid_game)
		return;
	for (int i = 0; i < 2; i++)
	{
		u8 flags[3] = { rfid_card[i].d4Seen, rfid_card[i].inserted, rfid_card[i].locked };
		LIBRETRO_SA(rfid_card[i].data, sizeof(rfid_card[i].data));
		LIBRETRO_SA(flags, 3);
	}
}

bool rfid_unserialize_v24(void **data, unsigned int *total_size)
{
	if (!rfid_game)
		return true;
	for (int i = 0; i < 2; i++)
	{
		u8 flags[3] = { 0, 0, 0 };
		LIBRETRO_USA(rfid_card[i].data, sizeof(rfid_card[i].data));
		if (!LIBRETRO_USA(flags, 3))
			return false;
		rfid_card[i].d4Seen = flags[0] != 0;
		rfid_card[i].inserted = flags[1] != 0;
		rfid_card[i].locked = flags[2] != 0;
	}
	return true;
}

// An older state: no card was in, and no reader was there for one
void rfid_state_before_v24(void)
{
	memset(rfid_card, 0, sizeof(rfid_card));
}

/* What each card out of its reader is - its file, or what the game last
 * wrote on it - in a state from V30 on (naomi_cards_serialize_v30()),
 * 256 bytes: a card put in after a state load is the one the state was
 * saved with. Read past when the game has no readers. */
void rfid_files_serialize_v30(void **data, unsigned int *total_size)
{
	LIBRETRO_SA(&rfid_file[0][0], sizeof(rfid_file));
}

bool rfid_files_unserialize_v30(void **data, unsigned int *total_size)
{
	static u8 files[2][128];
	if (!LIBRETRO_USA(&files[0][0], sizeof(files)))
		return false;
	if (rfid_game)
		memcpy(rfid_file, files, sizeof(rfid_file));
	return true;
}

struct maple_rfid_reader : maple_base
{
	virtual MapleDeviceType get_device_type()
	{
		return MDT_RFIDReaderWriter;
	}

	RfidCard& card()
	{
		return rfid_card[bus_id == 2 ? 1 : 0];
	}

	u32 status()
	{
		// b0: !card switch
		// b1: state=4	errors?
		// b2: state=5
		// b3: state=6
		// b4: state=7
		// b5: state=8
		// b6: card lock
		// when 0x40 trying to read the card
		u32 status = 1;
		if (card().inserted)
			status &= ~1;
		if (card().locked)
			status |= 0x40;
		return status;
	}

	virtual u32 dma(u32 cmd)
	{
		RfidCard& c = card();
		switch (cmd)
		{
		case MDC_DeviceRequest:
		case MDC_AllStatusReq:
			// custom function
			w32(0x00100000);
			// function flags
			w32(0);
			w32(0);
			w32(0);
			//1	area code
			w8(0xff);				// FF: Worldwide, 01: North America
			//1	direction
			w8(0);
			// Product name (totally made up)
			wstr("MAPLE/232C CONVERT BD", 30);
			// License (60)
			wstr(maple_sega_brand, 60);
			// Low-consumption standby current (2)
			w16(0x0069);	// 10.5 mA
			// Maximum current consumption (2)
			w16(0x0120);	// 28.8 mA
			return cmd == MDC_DeviceRequest ? MDRS_DeviceStatus : MDRS_DeviceStatusAll;

		case MDCF_GetCondition:
			w32(0x00100000); // custom function
			return MDRS_DataTransfer;

		case MDC_JVSSelfTest:
			w32(0);
			return MDRS_JVSSelfTestReply;

		case MDC_DeviceReset:
		case MDC_DeviceKill:
			return MDRS_DeviceReply;

		// 90	get status
		// d0, 91, a0, 91, a1 (read md5 in data, or the data itself after d4), d4, 91: read test
		// d9	lock
		// da	unlock
		// d0, 91, then b1 (offset, length in words, two bytes, data) and 91 for each part, c1, 91: write test
		case 0xD0:
			c.d4Seen = 0;
			// FALLTHROUGH
		case 0x90:
		case 0x91:
		case 0xA0:
		case 0xD4:
		case 0xC1:
			w32(status());
			if (cmd == 0xd4)
				c.d4Seen = 1;
			return 0xfe;

		case 0xA1:	// read card data
			DEBUG_LOG(MAPLE, "RFID card read (data? %d)\n", c.d4Seen);
			w32(status());
			// serial0 and serial1 only, unless after D4
			wptr(c.data, c.d4Seen ? sizeof(c.data) : 8);
			return 0xfe;

		case 0xD9:	// lock card
			c.locked = 1;
			w32(status());
			INFO_LOG(MAPLE, "RFID card %d locked\n", bus_id);
			return 0xfe;

		case 0xDA:	// unlock card
			c.locked = 0;
			c.inserted = 0;
			w32(status());
			NOTICE_LOG(MAPLE, "RFID card %d unlocked\n", bus_id);
			return 0xfe;

		case 0xB1:	// write to card
			w32(status());
			if (r_count() >= 4)
			{
				const u32 offset = r8() * 4;
				u32 size = r8() * 4;
				r8();
				r8();
				// (where and how much are the game's: no more than the card has, nor the frame)
				size = std::min(size, r_count());
				if (offset < sizeof(c.data))
				{
					size = std::min<u32>(size, sizeof(c.data) - offset);
					DEBUG_LOG(MAPLE, "RFID card write: offset 0x%x len %d\n", offset, size);
					rptr(c.data + offset, size);
					rfid_save_card(bus_id == 2 ? 1 : 0);
				}
			}
			return 0xfe;

		case 0xD1:	// decrement counter
			{
				int counter = r_count() >= 1 ? r8() : 0x03;
				switch (counter) {
				case 0x03:
					counter = 0;
					break;
				case 0x0c:
					counter = 1;
					break;
				case 0x30:
					counter = 2;
					break;
				case 0xc0:
					counter = 3;
					break;
				default:
					WARN_LOG(MAPLE, "Unknown counter selector %x\n", counter);
					counter = 0;
					break;
				}
				DEBUG_LOG(MAPLE, "RFID decrement %d\n", counter);
				c.data[19 - counter]--;
				rfid_save_card(bus_id == 2 ? 1 : 0);
				w32(status());
				return 0xfe;
			}

		default:
			INFO_LOG(MAPLE, "RFID: Unknown Maple command %x\n", cmd);
			return MDRE_UnknownCmd;
		}
	}

	virtual u32 RawDma(u32* buffer_in, u32 buffer_in_len, u32* buffer_out)
	{
		const u32 cmd = buffer_in[0] & 0xff;
		if (cmd == MDC_JVSGetId || cmd == MDC_JVSUploadFirmware || cmd == MDC_JVSCommand)
		{
			// what the bridge answers with frames of its own
			u32 out_len = 0;
			dma_buffer_out = (u8 *)buffer_out;
			dma_count_out = &out_len;
			dma_buffer_in = (u8 *)buffer_in + 4;
			dma_count_in = buffer_in_len - 4;
			if (cmd == MDC_JVSGetId)
			{
				w8(0x83);
				w8(0x00);
				w8(0x20);
				w8(0x07);
				wstr("315-6149    COPYRIGHT SEGA E", 28);
				w8(0x83);
				w8(0x00);
				w8(0x20);
				w8(0x05);
				wstr("NTERPRISES CO,LTD.  1998    ", 28);
			}
			else if (cmd == MDC_JVSUploadFirmware && dma_count_in >= 4 && dma_buffer_in[1] != 0xff)
			{
				// (the program is for the I/O board's processor, which this has not got)
				u8 sum = 0;
				for (u32 i = 0; i < 0x1C && i < dma_count_in; i++)
					sum += dma_buffer_in[i];
				w8(0x80);
				w8(0);
				w8(0x20);
				w8(0x01);
				w8(sum);
				w8(0);
				w8(0);
				w8(0);
				w8(MDRS_DeviceReply);
				w8(0x00);
				w8(0x20);
				w8(0x00);
			}
			else if (cmd == MDC_JVSUploadFirmware)
			{
				w8(MDRS_DeviceReply);
				w8(0x00);
				w8(0x20);
				w8(0x00);
			}
			else
			{
				w8(MDRE_UnknownCmd);
				w8(0x00);
				w8(0x20);
				w8(0x00);
			}
			return out_len;
		}
		const u32 len = maple_base::RawDma(buffer_in, buffer_in_len, buffer_out);
		// The recipient and the sender are not swapped in these replies (VF4 Final Tuned and Mushiking look)
		if ((buffer_out[0] & 0xff) == 0xfe)
		{
			u8 *p = (u8 *)buffer_out;
			std::swap(p[1], p[2]);
		}
		return len;
	}
};

/* The device of @type: made at @at - the memory of one of the same type,
 * gone, so that a state loaded makes nothing - or, with none, new */
#define MAPLE_MAKE(T) (at != NULL ? (maple_device *)new (at) T() : (maple_device *)new T())

maple_device* maple_CreateAt(MapleDeviceType type, void *at)
{
	maple_device* rv=0;
	switch(type)
	{
	case MDT_SegaController:
		if (settings.System == DC_PLATFORM_ATOMISWAVE)
			rv = MAPLE_MAKE(maple_atomiswave_controller);
		else
			rv = MAPLE_MAKE(maple_sega_controller);
		break;

	case MDT_TwinStick:
		rv = MAPLE_MAKE(maple_sega_twinstick);
		break;

	case MDT_AsciiStick:
		rv = MAPLE_MAKE(maple_ascii_stick);
		break;

	case MDT_Microphone:
		rv = MAPLE_MAKE(maple_microphone);
		break;

	case MDT_SegaVMU:
		rv = MAPLE_MAKE(maple_sega_vmu);
		break;

   case MDT_PurupuruPack:
      rv = MAPLE_MAKE(maple_sega_purupuru);
      break;

   case MDT_Keyboard:
		rv = MAPLE_MAKE(maple_keyboard);
		break;

   case MDT_Mouse:
		rv = MAPLE_MAKE(maple_mouse);
		break;

   case MDT_LightGun:
		if (settings.System == DC_PLATFORM_ATOMISWAVE)
			rv = MAPLE_MAKE(atomiswave_lightgun);
		else
			rv = MAPLE_MAKE(maple_lightgun);
		break;

	case MDT_NaomiJamma:
		rv = MAPLE_MAKE(maple_naomi_jamma);
		break;

	case MDT_RFIDReaderWriter:
		rv = MAPLE_MAKE(maple_rfid_reader);
		break;

	default:
		ERROR_LOG(MAPLE, "Invalid device type %d\n", type);
		die("Invalid maple device type");
		break;
	}

	return rv;
}

u32 maple_dev_dma(maple_device *dev, u32 *in, u32 in_len, u32 *out)
{
	return dev->RawDma(in, in_len, out);
}

bool maple_dev_has_slots(maple_device *dev)
{
	return maple_HasSlots(dev->get_device_type());
}

bool maple_dev_lightgun_pos(maple_device *dev)
{
	return dev->get_lightgun_pos();
}

void maple_dev_flush_save(maple_device *dev)
{
	dev->FlushSave();
}

maple_device* maple_Create(MapleDeviceType type)
{
	return maple_CreateAt(type, NULL);
}

maple_device *maple_dev_create(MapleDeviceType type)
{
	return maple_Create(type);
}

void maple_dev_delete(maple_device *dev)
{
	delete dev;
}

void *maple_dev_destruct(maple_device *dev)
{
	dev->~maple_device();
	return dev;
}

maple_device *maple_dev_create_at(MapleDeviceType type, void *at)
{
	return maple_CreateAt(type, at);
}

void maple_dev_setup(maple_device *dev, u32 addr, struct mcfg_map *config)
{
	dev->Setup(addr);
	dev->config = config;
	dev->OnSetup();
}

MapleDeviceType maple_dev_type(maple_device *dev)
{
	return dev->get_device_type();
}

u32 maple_dev_bus_id(const maple_device *dev)
{
	return dev->bus_id;
}

bool maple_dev_serialize(maple_device *dev, void **data, unsigned int *total_size)
{
	return dev->maple_serialize(data, total_size);
}

bool maple_dev_unserialize(maple_device *dev, void **data, unsigned int *total_size)
{
	return dev->maple_unserialize(data, total_size);
}
