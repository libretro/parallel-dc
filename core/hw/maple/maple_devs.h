#pragma once
#include <math.h>
#include "types.h"

/* WARNING: changing the order of this enum or inserting new values will break savestates compatibility */
enum MapleDeviceType
{
   MDT_SegaController,
   MDT_SegaVMU,
   MDT_Microphone,
   MDT_PurupuruPack,
   
   MDT_AsciiStick,
   MDT_Keyboard,
   MDT_Mouse,
   MDT_LightGun,
   MDT_TwinStick,

   MDT_NaomiJamma,

   MDT_None,

   /* IC card reader/writer of VF4 and Mushiking (after MDT_None: it came later) */
   MDT_RFIDReaderWriter,

   /* Add new device types before this comment */
   MDT_Count
};
typedef enum MapleDeviceType MapleDeviceType;

enum NAOMI_KEYS
{
	NAOMI_START_KEY = 1 << 15,
	NAOMI_SERVICE_KEY = 1 << 14,

	NAOMI_UP_KEY = 1 << 13,
	NAOMI_DOWN_KEY = 1 << 12,
	NAOMI_LEFT_KEY = 1 << 11,
	NAOMI_RIGHT_KEY = 1 << 10,

	NAOMI_BTN0_KEY = 1 << 9,
	NAOMI_BTN1_KEY = 1 << 8,
	NAOMI_BTN2_KEY = 1 << 7,
	NAOMI_BTN3_KEY = 1 << 6,
	NAOMI_BTN4_KEY = 1 << 5,
	NAOMI_BTN5_KEY = 1 << 4,
	NAOMI_BTN6_KEY = 1 << 3,
	NAOMI_BTN7_KEY = 1 << 2,
	NAOMI_BTN8_KEY = 1 << 16,

	NAOMI_TEST_KEY = 1 << 1,

	/* Not an actual button */
	NAOMI_COIN_KEY = 1 << 0,
	/* Nor this: a card put in the game's card reader */
	NAOMI_INSERT_CARD_KEY = 1 << 17
};

enum AWAVE_KEYS
{
	AWAVE_START_KEY = 1 << 3,

	AWAVE_BTN0_KEY  = 1 << 2,
	AWAVE_BTN1_KEY  = 1 << 1,
	AWAVE_BTN2_KEY  = 1 << 0,
	AWAVE_BTN3_KEY  = 1 << 10,
	AWAVE_BTN4_KEY  = 1 << 9,

	AWAVE_UP_KEY    = 1 << 4,
	AWAVE_DOWN_KEY  = 1 << 5,
	AWAVE_LEFT_KEY  = 1 << 6,
	AWAVE_RIGHT_KEY = 1 << 7,

	AWAVE_SERVICE_KEY = 1 << 13,
	AWAVE_TEST_KEY 	  = 1 << 14,

	/* Not an actual button */
	AWAVE_COIN_KEY    = 1 << 15,
	AWAVE_TRIGGER_KEY    = 1 << 12
};

/* The maple bus's commands and answers */
enum MapleDeviceCommand
{
	MDC_DeviceRequest   = 0x01, /* 7 words.Note : Initialises device */
	MDC_AllStatusReq    = 0x02, /* 7 words + device dependent ( seems to be 8 words) */
	MDC_DeviceReset     = 0x03, /* 0 words */
	MDC_DeviceKill      = 0x04, /* 0 words */
	MDC_DeviceStatus    = 0x05, /* Same as MDC_DeviceRequest ? */
	MDC_DeviceAllStatus = 0x06, /* Same as MDC_AllStatusReq ? */

	/* Various Functions */
	MDCF_GetCondition   = 0x09, /* FT */
	MDCF_GetMediaInfo   = 0x0A, /* FT,PT,3 pad */
	MDCF_BlockRead      = 0x0B, /* FT,PT,Phase,Block # */
	MDCF_BlockWrite     = 0x0C, /* FT,PT,Phase,Block #,data ... */
	MDCF_GetLastError   = 0x0D, /* FT,PT,Phase,Block # */
	MDCF_SetCondition   = 0x0E, /* FT,data ... */
	MDCF_MICControl     = 0x0F, /* FT,MIC data ... */
	MDCF_ARGunControl   = 0x10, /* FT,AR-Gun data ... */

	MDC_JVSUploadFirmware = 0x80, /* JVS bridge firmware */
	MDC_JVSGetId		 = 0x82,
	MDC_JVSSelfTest		 = 0x84, /* JVS Self Test */
	MDC_JVSCommand		 = 0x86  /* JVS I/O */
};

enum MapleDeviceRV
{
	MDRS_JVSNone		 = 0x00, /* No reply, used for multiple JVS I/O boards */

	MDRS_DeviceStatus    = 0x05, /* 28 words */
	MDRS_DeviceStatusAll = 0x06, /* 28 words + device dependent data */
	MDRS_DeviceReply     = 0x07, /* 0 words */
	MDRS_DataTransfer    = 0x08, /* FT,depends on the command */

	MDRE_UnknownFunction = 0xFE, /* 0 words */
	MDRE_UnknownCmd      = 0xFD, /* 0 words */
	MDRE_TransmitAgain   = 0xFC, /* 0 words */
	MDRE_FileError       = 0xFB, /* 1 word, bitfield */
	MDRE_LCDError        = 0xFA, /* 1 word, bitfield */
	MDRE_ARGunError      = 0xF9, /* 1 word, bitfield */

	MDRS_JVSSelfTestReply= 0x85, /* JVS I/O SelfTest */
	MDRS_JVSReply		 = 0x87  /* JVS I/O */
};

#define MAPLE_PORTS 4
#ifdef __cplusplus
extern "C" {
#endif
/* Maple device configuration for mcfg_CreateDevices() */
extern MapleDeviceType maple_devices[MAPLE_PORTS];

extern bool enable_naomi_15khz_dipswitch;
/* The BIOS is the multi-region one, which takes the region from the DIP
 * switches (libretro.cpp) */
extern bool naomi_bios_multiregion;
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
struct mcfg_map;

struct maple_device
{
	u8 maple_port;          //raw maple port
	u8 bus_port;            //0 .. 5
	u8 bus_id;              //0 .. 3
	char logical_port[3];  //A0, etc
	struct mcfg_map* config;	/* its slot's (maple_cfg.c) */

	//fill in the info
	void Setup(u32 prt);

	virtual void OnSetup(){};
	/* Write out what the game changed, off the emulation path. */
	virtual void FlushSave(){};
	virtual ~maple_device();
	u32 Dma(u32 Command,u32* buffer_in,u32 buffer_in_len,u32* buffer_out,u32& buffer_out_len);
	virtual u32 RawDma(u32* buffer_in, u32 buffer_in_len, u32* buffer_out) = 0;
	virtual bool maple_serialize(void **data, unsigned int *total_size){return true;};
	virtual bool maple_unserialize(void **data, unsigned int *total_size){return true;};
	virtual MapleDeviceType get_device_type() = 0;
	virtual bool get_lightgun_pos() { return false; };
};

maple_device* maple_Create(MapleDeviceType type);
maple_device* maple_CreateAt(MapleDeviceType type, void *at);
#define SIZE_OF_MIC_DATA	480 //ALSO DEFINED IN SipEmulator.java
int get_mic_data(u8* buffer); //implemented in Android.cpp
int push_vmu_screen(u8* buffer); //implemented in Android.cpp




/* What C sees of the devices */
#include "maple_if.h"
#endif
