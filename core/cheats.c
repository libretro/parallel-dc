/*
    Created on: Sep 23, 2019

	Credits for cheats: Esppiral, S4pph4rad, yzb37859365, Shenmue_Trilogy, Radaron, Virgin KLM, Joel, Zorlon,
		ELOTROLADO.NET, SEGARETRO.ORG, Sakuragi @ emutalk.net
	Copyright 2019 flyinghead

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
 */
#include "cheats.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/naomi/naomi_cart.h"
#include "reios/reios.h"
#include "paths.h"
#include <string.h>
#include <file/file_path.h>
#include <retro_miscellaneous.h>

static const Cheat widescreen_cheats[] =
{
		{ "T36803N   ", NULL,    { 0xBC2CC, 0 }, { 0xC00 } },			/* 102 Dalmatians (USA) */
		{ "T36813D 50", NULL,    { 0xBDE8C, 0 }, { 0xC00 } },			/* 102 Dalmatians (PAL) */
		{ "MK-51064  ", NULL,    { 0x39EFF4, 0 }, { 0x43700000 } },		/* 18 wheeler (USA) */
		{ "MK-5106450", NULL,    { 0x39EFF4, 0 }, { 0x43700000 } },		/* 18 wheeler (PAL) */
		{ "HDR-0080  ", NULL,    { 0x6625C0, 0 }, { 0x43700000 } },		/* 18 wheeler (JP) */
		{ "T-9708N   ", NULL,    { 0x0AD5B0, 0x112E00, 0 }, { 0x4A2671C4, 0x401CCCCD } },	/* 4 Wheel Thunder (USA) */
		{ "MK-5119050", NULL,    { 0x104A34, 0 }, { 0x3F52CCCD } },		/* 90 Minutes - Sega Championship Football (PAL) */
		{ "T40209D 50", NULL,    { 0x2A8750, 0 }, { 0x3F400000 } },		/* AeroWings 2 (PAL) */
		{ "T34101M   ", NULL,    { 0x24CF20, 0x24CEEC, 0 }, { 0x3FAAAAAB, 0x43F00000, 0 } },	/* Animastar (JP) */
		{ "MK-51171  ", NULL,    { 0xCA20B8, 0 }, { 0x43700000 } },		/* Alien Front Online (USA) */
		{ "T2101M    ", NULL,    { 0x13E8B4, 0x13E8E4, 0 }, { 0x43F00000, 0x3F400000 } },	/* Berserk (JP) */
		{ "T13001D 18", NULL,    { 0x2DAC44, 0 }, { 0x3F400000 } },		/* Blue Stinger (En, De) (PAL) */
		{ "xxxxxxxxxx", NULL,    { 0x2DAB84, 0 }, { 0x3F400000 } },		/* Blue Stinger (Fr) (PAL) */
		{ "T13001D-05", NULL,    { 0x2D9C84, 0 }, { 0x3F400000 } },		/* Blue Stinger (Es, It?) (PAL) */
		{ "T13001N   ", NULL,    { 0x2D9CA4, 0 }, { 0x3F400000 } },		/* Blue Stinger (USA) */
		{ "HDR-0003  ", NULL,    { 0x2D6D80, 0 }, { 0x3F400000 } },		/* Blue Stinger (JP) */
		{ "T42903M   ", NULL,    { 0x286E2C, 0x286E5C, 0 }, { 0x43F00000, 0x3F400000 } },	/* Bomber Hehhe! (JP) */
		{ "MK-51065  ", NULL,    { 0x387B84, 0x387BB4, 0 }, { 0x43F00000, 0x3F400000 } },	/* Bomberman Online (USA) */
		{ "T6801M    ", NULL,    { 0x042F3C, 0 }, { 0x4384BC09 } },		/* Buggy Heat (JP) */
		{ "T46601D 05", NULL,    { 0xBB3D14, 0 }, { 0x440A7C9A } },		/* Cannon Spike (PAL) */
		{ "T1215N    ", NULL,    { 0xBB3C74, 0 }, { 0x440A7C9A } },		/* Cannon Spike (USA) */
		{ "HDR-0115  ", NULL,    { 0x10ACFC, 0 }, { 0x3F400000 } },		/* Cardcaptor Sakura: Tomoyo no Video Daisakusen (JP) */
		{ "T44901D 50", NULL,    { 0xB0109C, 0xB010CC, 0 }, { 0x43F00000, 0x3F400000 } },	/* Carrier (PAL) */
		{ "T5701N    ", NULL,    { 0xAFD93C, 0xAFD96C, 0 }, { 0x43F00000, 0x3F400000 } },	/* Carrier (USA) */
		/* Capcom vs. SNK Pro (JP) */
		/* You can see the end of the backgrounds on each side. The HUD is shifted to the right. */
		/* Code 2 sets HUD and movements of the characters to a 4:3 zone */
		{ "T1247M    ", NULL,    { 0x1E8238, 0x3D3658, 0x3D3628, 0 }, { 0x43F00000, 0x43D20000, 0x43700000 } },
		/* Capcom vs. SNK 2 (JP) */
		/* You can see the end of the backgrounds on each side. */
		{ "T1249M    ", NULL,    { 0x39BFD8, 0x34E9E8, 0x34EFBC, 0 }, { 0x43700000, 0x3F400000, 0x3F400000 } },
		{ "T44902D 50", NULL,    { 0x1DBFF8, 0 }, { 0x43700000 } },		/* Charge ‘n Blast (PAL) */
		{ "T4402M    ", NULL,    { 0x1DA9E0, 0 }, { 0x43700000 } },		/* Charge ‘n Blast (JP) */
		{ "MK-5104950", NULL,    { 0x2D2D40, 0x2D2D70, 0 }, { 0x3F400000, 0xC2700000 } },	/* ChuChu Rocket! (PAL) */
		{ "T44903D 50", NULL,    { 0x315300, 0x315334, 0 }, { 0x43F00000, 0x3FAAAAAB } },	/* Coaster Works (PAL) */
		/* Confidential Mission (PAL) 022F0D58 43700000 - Only works on real Dreamcast */
		{ "T36901M   ", NULL,    { 0x1C8A98, 0 }, { 0x3F400000 } },		/* Cool Boarders (JP) */
		{ "T3106M    ", NULL,    { 0x60B59C, 0 }, { 0x3F400000 } },		/* Cool Cool Toon (JP) */
		{ "HDR-0176  ", NULL,    { 0x240FAC, 0 }, { 0x3F400000 } },		/* Cosmic Smash (JP) */
		{ "MK-51035  ", " U      ", { 0x2B08B0, 0 }, { 0x43700000 } },		/* Crazy Taxi (USA) */
		{ "MK-51035  ", "  E     ", { 0x2B3410, 0 }, { 0x43700000 } },		/* Crazy Taxi (PAL) */
		{ "MK-5113650", NULL,    { 0x2BFB70, 0 }, { 0x43700000 } },		/* Crazy Taxi 2 (PAL) */
		{ "MK-51136  ", NULL,    { 0x2BDDD0, 0 }, { 0x43700000 } },		/* Crazy Taxi 2 (USA) */
/* { "HDR-0159  ", NULL,    { 0x2FBBD0, 0 }, { 0x43700000 } },		// Crazy Taxi 2 (JP) not working */
		{ "T13004N   ", NULL,    { 0x016D94, 0 }, { 0x44234E73 } },		/* Cyber Troopers - Virtual On - Oratorio Tangram (USA) */
		{ "HDR-0040  ", NULL,    { 0x016D94, 0 }, { 0x44234E73 } },		/* Cyber Troopers - Virtual On - Oratorio Tangram (JP) */
		/* D2 (USA) */
		{ "MK-51036  ", NULL,    { 0x4B5CF4, 0x4B5CC4, 0x3E92A0, 0x3E92A8, 0x3E92C0, 0x3E92C8 },
				{ 0x3F400000, 0x43F00000, 0, 0, 0, 0 } },
		/* D2 (JP) */
		{ "T30006M   ", NULL,    { 0x4CF42C, 0x4CF45C, 0x3E1A36, 0x3E1A34, 0x3E1A3C, 0x3E1A54, 0x3E1A5C, 0 },
				{ 0x43F00000, 0x3F400000, 0x08010000, 0, 0, 0, 0 } },
		{ "MK-5103750", NULL,    { 0x1FE270, 0 }, { 0x43700000 }, { 0x43A00000 } },	/* Daytona USA (PAL) */
		{ "MK-51037  ", NULL,    { 0x1FC6D0, 0 }, { 0x43700000 }, { 0x43A00000 } },	/* Daytona USA (USA) */
		{ "T9501N-50 ", NULL,    { 0x9821D4, 0 }, { 0x3F400000 } },		/* Deadly Skies (PAL) */
		{ "T8116D  50", NULL,    { 0x2E5530, 0 }, { 0x43700000 } },		/* Dead or Alive 2 (PAL) */
		{ "T3601N    ", NULL,    { 0x2F0670, 0 }, { 0x43700000 } },		/* Dead or Alive 2 (USA) */
		{ "T3602M    ", NULL,    { 0x2FF798, 0 }, { 0x43700000 } },		/* Dead or Alive 2 (JP) */
		{ "T3601M    ", NULL,    { 0x2FBBD0, 0 }, { 0x43700000 } },		/* Dead or Alive 2: Limited Edition (JP) */
		{ "T2401N    ", NULL,    { 0x8BD5B4, 0x8BD5E4, 0 }, { 0x43F00000, 0x3F400000 } },	/* Death Crimson OX (USA) */
		{ "T23201M   ", NULL,    { 0x819F44, 0x819F74, 0 }, { 0x43F00000, 0x3F400000 } },	/* Death Crimson 2 (JP) */
		{ "T17714D50 ", NULL,    { 0x0D2ED0, 0x0D2ED4, 0 }, { 0x3FAAAAAB, 0x3F400000 } },	/* Donald Duck: Quack Attack (PAL) (Code 1 corrects the HUD) */
		{ "T12503D-50", NULL,    { 0x49CB24, 0 }, { 0x3F0CCCCD } },		/* Dragons Blood (PAL) */
		{ "T17716D 50", NULL,    { 0xF97F40, 0x08DCF8, 0 }, { 0x00000168, 0 } },		/* Dragonriders: Chronicles of Pern (PAL) */
		{ "T17720N   ", NULL,    { 0xF97F40, 0x08DCF4, 0 }, { 0x00000168, 0 } },		/* Dragonriders: Chronicles of Pern (USA) (Code 1 removes black bars) */
		{ "MK-5101350", NULL,    { 0x4FCBC0, 0 }, { 0x43700000 } },		/* Dynamite Cop  (PAL) */
		{ "MK-51013  ", NULL,    { 0x4FCBC0, 0 }, { 0x43700000 } },		/* Dynamite Cop  (USA) */
		{ "HDR-0020  ", NULL,    { 0x4FA848, 0 }, { 0x43700000 } },		/* Dynamite Deka 2 (JP) */
		/* Draconus: Cult of the Wyrm (USA) */
		/* Code 1-2 increases drawing distance */
		{ "T40203N   ", NULL,    { 0x49D7F4, 0x49D8CC, 0x49D6F8, 0 }, { 0x3F07C3BB, 0x447A0000, 0x3FAAAAAB } },
		/* Ecco the Dolphin: Defender of the Future (PAL) */
		{ "MK-5103350", NULL,    { 0x275418, 0x040E68, 0x040D1C, 0 }, { 0x49D9A5DA, 0x3F100000, 0x3F100000 } },
		{ "T17705D 05", NULL,    { 0x304870, 0 }, { 0x3F400000 } },		/* Evolution - The World of Sacred Device (PAL) */
		{ "T45005D 50", NULL,    { 0x36CE5C, 0x36CE8C, 0 }, { 0x43F00000, 0x3F400000 } },	/* Evolution 2 - Far Off Promise (PAL) */
		{ "T1711N    ", NULL,    { 0x36C76C, 0x36C73C, 0 }, { 0x3F400000, 0x43F00000 } },	/* Evolution 2 - Far Off Promise (USA) */
		{ "T8118D  50", NULL,    { 0x2C6B7C, 0 }, { 0x00004000 } },		/* Ferrari F355 Challenge (PAL) vga mode only */
		{ "HDR-0100  ", NULL,    { 0x3235D4, 0 }, { 0x00004000 } },		/* Ferrari F355 Challenge (JP) vga mode only */
		{ "MK-5115450", NULL,    { 0x3D3B10, 0 }, { 0x43700000 } },		/* Fighting Vipers 2 (PAL) */
		{ "HDR-0133  ", NULL,    { 0x3D3AF0, 0 }, { 0x43700000 } },		/* Fighting Vipers 2 (JP) */
		{ "T18805M   ", NULL,    { 0x1A39C0, 0 }, { 0x3F400000 } },		/* Fire Pro Wrestling D (JP) */
		{ "MK-51114  ", "  E     ", { 0x132DD8, 0xA26CA8, 0xA26738, 0xA275B8, 0xA26AD8, 0xA26908, 0 },
				{ 0x3F400000, 0x3F400000, 0x3F400000, 0x3F400000, 0x3F400000, 0x3F400000 } },	/* Floigan Bros. Ep. 1 (PAL) */
		{ "T34201M   ", NULL,    { 0x586290, 0x586260, 0 }, { 0x3F400000, 0x43F00000 } },	/* Frame Gride (JP) */
		{ "T-8113D-50", NULL,    { 0x55A354, 0 }, { 0x3FAAAAAB } },		/* Fur Fighters (PAL) */
		{ "T-8107N   ", NULL,    { 0x55A374, 0 }, { 0x3FAAAAAB } },		/* Fur Fighters (USA) */
		{ "T9707D  50", NULL,    { 0x2E7CD0, 0x2E7D04, 0x2E7D10, 0x2E7D0C, 0 },
				{ 0x43BEAE39, 0x3DCCCCCD , 0x44A00000, 0xC4200000 } },		/* Gauntlet Legends (PAL) */
		{ "HDR-0004  ", NULL,    { 0x364A64, 0 }, { 0x43BA0000 } },		/* Godzilla Generations (JP) */
		{ "HDR-0047  ", NULL,    { 0x7C3F24, 0x7C3F54, 0 }, { 0x43F00000, 0x3F400000 } },	/* Godzilla Generations Maximum Impact (JP) */
		{ "T41501M   ", NULL,    { 0x282604, 0x282634, 0 }, { 0x43F00000, 0x3F400000 } },	/* Golem no Maigo (aka The Lost Golem) (JP) */
		{ "T1219M    ", NULL,    { 0xBC3C94, 0 }, { 0x440A7C9A } },		/* Gun Spike (JP) */
		{ "T13301N   ", NULL,    { 0x88E780, 0 }, { 0x3F400000 } },		/* Gundam Side Story (USA) */
/* { "T11001N   ", NULL,    { 0xC3D6BC, 0xD204A8, 0xD32FC8, 0xC7CF84, 0xD20548, 0 }, */
/* { 0x00363031, 0x00000356, 0x00000280, 0x00000280, 0x00000280 } },		// Half-Life. Not working */
		{ "MK-5104150", NULL,    { 0x23FCC4, 0 }, { 0x44558000 } },		/* Headhunter (PAL) */
		/* (not House of the Dead 2: the gun's coordinates come out wrong) */
/* { "T38706M   ", NULL,    { 0xC0CFA0, 0 }, { 0x3F400000 } },		// Ikaruga (JP) not working */
		{ "T46001N   ", NULL,    { 0x1C8A98, 0 }, { 0x3F400000 } },		/* Illbleed (USA) */
		{ "T44904D 50", NULL,    { 0x18C15C, 0x18C18C, 0 }, { 0x43F00000, 0x3F400000 } },	/* Iron Aces (PAL) */
		{ "MK-51058  ", NULL,    { 0x32E0FC, 0x32E12C, 0 }, { 0x43F00000, 0x3F400000 } },	/* Jet Grind Radio (USA) */
		{ "MK-5105850", NULL,    { 0x32F9EC, 0x32F9BC, 0 }, { 0x3F400000, 0x43F00000 } },	/* Jet Set Radio (PAL) */
		{ "HDR-0078  ", NULL,    { 0x327A8C, 0x327A5C, 0 }, { 0x3F400000, 0x43F00000 } },	/* Jet Set Radio (De La) (JP) */
		{ "T22902D 50", NULL,    { 0x278508, 0 }, { 0x43700000 } },		/* Kao The Kangaroo (PAL) */
		{ "T22903N   ", NULL,    { 0x2780A8, 0 }, { 0x43700000 } },		/* Kao The Kangaroo (USA) */
		{ "T47803M   ", NULL,    { 0x0FDFAC, 0 }, { 0x3F400000 } },		/* Karous (JP) */
/* { "T41901N   ", NULL,    { 0x53F580, 0xEFB748, 0xEFB750, 0 }, { 0xC4200000, 0x43A00000, 0x43200000 } },	// KISS Psycho Circus – The Nightmare Child (USA) */
		{ "T2501M    ", NULL,    { 0x24A878, 0x24A8A8, 0 }, { 0x43F00000, 0x3F400000 } },	/* Langrisser Millenium (JP) */
		{ "T15111D 50", NULL,    { 0x29B90C, 0 }, { 0x3F400000 } },		/* Le Mans 24 Hours (PAL) */
		{ "T15116N   ", NULL,    { 0x2198EC, 0 }, { 0x3F400000 } },		/* Looney Tunes: Space Race (USA) */
		{ "MK-5105050", NULL,    { 0x33818C, 0 }, { 0x3FA66666 } },		/* Maken X (PAL) */
		{ "MK-51050  ", NULL,    { 0x30CB4C, 0 }, { 0x3F400000 } },		/* Maken X (USA) */
		{ "T1212N    ", NULL,    { 0x2D6B18, 0x268390, 0x268ED8, 0x268934, 0x26947C, 0x269A20, 0x269FC4, 0 },
				{ 0x43700000, 0x3F400000, 0x3F400000, 0x3F400000, 0x3F400000, 0x3F400000, 0x3F400000 } },		/* Marvel vs. Capcom 2 (USA) */
		{ "T1215M    ", NULL,    { 0x2FE2C0, 0x28FB38, 0x290680, 0x2900DC, 0x290C24, 0x2911C8, 0x29176C, 0 },
				{ 0x43700000, 0x3F400000, 0x3F400000, 0x3F400000, 0x3F400000, 0x3F400000, 0x3F400000 } },		/* Marvel vs. Capcom 2 (JP) */
		{ "T41402N   ", NULL,    { 0x551B80, 0 }, { 0x3F400000 } },		/* Max Steel - Covert Missions (USA) */
		{ "MK-5102250", "V1.001",   { 0x107FDC, 0x11253C, 0 }, { 0x3F99999A, 0x3F900000 } },	/* Metropolis Street Racer (v1.001) (PAL) */
		{ "MK-5102250", "V1.009",   { 0x106B5C, 0x1111F4, 0 }, { 0x3F99999A, 0x3F900000 } },	/* Metropolis Street Racer (v1.009) (PAL) */
		{ "MK-51012  ", NULL,    { 0x10A01C, 0x1146FC, 0 }, { 0x3F99999A, 0x3F900000 } },	/* Metropolis Street Racer (USA) */
		{ "T0000M    ", "MILLENNIUM RACER - Y2K FIGHTERS                                                                                                 ",
									{ 0x1CAEAC, 0x1CAEDC, 0 }, { 0x43F00000, 0x3F400000 } },	/* Millenium Racer Y2K Fighters */
		{ "T1221M    ", NULL,    { 0x426B74, 0 }, { 0x43F00000 } },		/* Moero! Justice Gauken (JP) */
		{ "T9701D    ", NULL,    { 0x31290C, 0 }, { 0x3F400000 } },		/* Mortal Kombat Gold (PAL) */
		{ "T-9701N   ", NULL,    { 0x337B8C, 0 }, { 0x3FA66666 } },		/* Mortal Kombat Gold (USA) */
		{ "T7020D    ", NULL,    { 0x1A5D40, 0x1A5D18, 0x1A5D38, 0x1A5D34, 0x1A5D30, 0x1A5D3C, 0x1A5D0C, 0x1A5D08, 0x1ACB78, 0 },
				{ 0x3F59999A, 0x44084000, 0x3FB1EB85, 0x3FAAAAAB, 0x3F800000, 0x3FA66666, 0xC31B0000, 0x42700000, 0x44160000 } },		/* Mr Driller (PAL) */
		{ "T7604M    ", NULL,    { 0x43BDA4, 0x43BDD8, 0x4319E8, 0x431A08, 0x431A28, 0x431A48, 0 },
				{ 0x43700000, 0x3F9AF5C2, 0x42200000, 0x42200000, 0x438C0000, 0x438C0000 } },	/* Nanatsu no Hikan - Senritsu no Bishou (JP) */
		{ "HDR-0079  ", NULL,    { 0x353744, 0x353774, 0x353778, 0 },
				{ 0x43F00000, 0x3F400000, 0x3F800000 } },					/* Napple Tale: Arsia in Daydream (JP) */
/* crash		{ "MK-51178  ", NULL,    { 0x23AF00, 0x23B160, 0x144D40, 0x2105B4, 0x705B40, 0 }, */
/* { 0xBFAAAAAB, 0xBFAAAAAB, 0xBFAAAAAB, 0xBFAAAAAB, 0x44558000 } },	// NBA 2K2 */
		/* (not Nightmare Creatures II: it does not work) */
		{ "MK-5110250", NULL,    { 0x87B5A4, 0 }, { 0x43700000 } },		/* Outtrigger (PAL) */
		{ "HDR-0118  ", NULL,    { 0x83E284, 0 }, { 0x43700000 } },		/* Outtrigger (JP) */
		{ "MK-51102  ", NULL,    { 0x83E284, 0 }, { 0x43700000 } },		/* Outtrigger (US) */
		{ "T15103D 50", NULL,    { 0x1EEE78, 0 }, { 0x3F400000 } },		/* PenPen (PAL) */
		{ "T17001M   ", NULL,    { 0x1C3828, 0 }, { 0x3F400000 } },		/* PenPen TriIcelon (JP) */
		{ "MK-5110050", NULL,    { 0x548E04, 0x0923C0, 0 }, { 0x43E80000, 0x3F966666 } },	/* Phantasy Star Online (PAL) TODO */
		{ "MK-51100  ", NULL,    { 0x548E84, 0x0925A4, 0 }, { 0x43D20000, 0x3FA66666 } },	/* Phantasy Star Online (USA) TODO */
		{ "HDR-0129  ", NULL,    { 0x57E344, 0x091794, 0 }, { 0x43D20000, 0x3FA66666 } },	/* Phantasy Star Online (JP) TODO */
		{ "MK-5119350", NULL,    { 0x57552C, 0x0A2DD4, 0 }, { 0x43F00000, 0x3F911111 } },	/* Phantasy Star Online Ver. 2 (PAL) TODO */
		{ "MK-51193  ", NULL,    { 0x0A3138, 0x58C8CC, 0 }, { 0x3F911111, 0x43F00000 } },	/* Phantasy Star Online Ver. 2 (USA) TODO */
		{ "T1207N    ", NULL,    { 0x7D463C, 0 }, { 0x3F400000 } },		/* Plasma Sword - Nightmare of Bilstein (USA) */
		{ "T36801D 50", NULL,    { 0x7FD3C8, 0 }, { 0x43700000 } },		/* Power Stone (PAL) */
		{ "T1201N    ", NULL,    { 0x7FCEE8, 0 }, { 0x43700000 } },		/* Power Stone (USA) */
		{ "T36812D 50", NULL,    { 0x868BA8, 0 }, { 0x43700000 } },		/* Power Stone 2 (PAL) */
		{ "T1211N    ", NULL,    { 0x8689A8, 0 }, { 0x43700000 } },		/* Power Stone 2 (USA) */
		{ "T1218M    ", NULL,    { 0x849AA0, 0 }, { 0x43700000 } },		/* Power Stone 2 (JP) */
		{ "T7022D  50", NULL,    { 0x33414C, 0 }, { 0x43F00000 } },		/* Project Justice (PAL) */
		{ "T1219N    ", NULL,    { 0x33374C, 0 }, { 0x43F00000 } },		/* Project Justice (USA) */
		{ "MK-51162  ", NULL,    { 0x204308, 0 }, { 0x3FE38E39 } },		/* Propeller Arena */
		{ "T-8106D-50", NULL,    { 0x7DA39C, 0 }, { 0x3F400000 } },		/* Psychic Force 2012 (PAL) */
		{ "T9901M    ", NULL,    { 0x1AD848, 0 }, { 0x3F400000 } },		/* Rainbow Cotton (JP) */
		{ "T9711D  50", NULL,    { 0x1574D8, 0 }, { 0x43700000 } },		/* Ready 2 Rumble Boxing Round 2 (PAL) */
		{ "T7012D  05", NULL,    { 0x4FF93C, 0x4FF96C, 0 }, { 0x43F00000, 0x3F400000 } },	/* Record of Lodoss War (En) (PAL) */
		{ "xxxxxxxxxx", NULL,    { 0x4FF25C, 0x4FF28C, 0 }, { 0x43F00000, 0x3F400000 } },	/* Record of Lodoss War (De) (PAL) */
		{ "T7012D  09", NULL,    { 0x50499C, 0x5049CC, 0 }, { 0x43F00000, 0x3F400000 } },	/* Record of Lodoss War (Fr) (PAL) */
		{ "MK-5102151", NULL,    { 0x3511A0, 0 }, { 0x3FC58577 } },		/* Red Dog (PAL) */
		{ "HDR-0074  ", NULL,    { 0x1FF60C, 0x1FF610, 0x1FF5DC, 0 }, { 0x3F400000, 0x3F800000, 0x43DC0000 } },	/* Rent a Hero n°1 */
		/* Resident Evil: Code Veronica (De) (PAL) */
		/* Code 1-4 removes the black bars on top and bottom in FMV */
		{ "xxxxxxxxxx", NULL,    { 0x32A380, 0x383E18, 0x383E38, 0x383E58, 0x383E78, 0 },
				{ 0x3F400000, 0x43F00000, 0, 0x43F00000, 0 } },
		/* Resident Evil: Code Veronica (Fr) (PAL) */
		/* Code 1-4 removes the black bars on top and bottom in FMV */
		{ "T36806D 09", NULL,    { 0x32A380, 0x383E18, 0x383E38, 0x383E58, 0x383E78, 0 },
				{ 0x3F400000, 0x43F00000, 0, 0x43F00000, 0 } },
		/* Resident Evil: Code Veronica (Sp) (PAL) */
		{ "xxxxxxxxxx", NULL,    { 0x32A3A0, /* 0x383E18, 0x383E38, 0x383E58, 0x383E78, */ 0 },
				{ 0x3F400000, 0x43F00000, 0, 0x43F00000, 0 } },
		/* Resident Evil: Code Veronica (USA) */
		/* Code 1-4 removes the black bars on top and bottom in FMV */
		{ "T1204N    ", "RESIDENT EVIL CODE VERONICA                                                                                                     ",
				{ 0x329E40, 0x3838D8, 0x3838F8, 0x383918, 0x383938, 0 },
				{ 0x3F400000, 0x43F00000, 0, 0x43F00000, 0 } },
		/* Biohazard: Code Veronica: Kanzenban (JP) */
		{ "T1240M    ", "BIOHAZARD CODE VERONICA PLUS                                                                                                    ",
				{ 0x344AE1, 0 }, { 0x003F6000 } },
		/* Resident Evil: Code Veronica X Kanzenban (English translation) */
		{ "T1204N    ", "RESIDENT EVIL CODE VERONICA X                                                                                                   ",
				{ 0x344AE1, 0 }, { 0x003F6000 } },
		{ "T8107D  50", NULL,    { 0x0464FC, 0x046210, 0 }, { 0x3A888889, 0x44200000 } },	/* Re-Volt (PAL) Code 1 is a render fix */
		{ "MK-5119250", NULL,    { 0x0C5EB4, 0 }, { 0x3A888889 } },		/* Rez (PAL) */
		{ "T15122N   ", NULL,    { 0x8E7A80, 0x8E7AB4, 0 }, { 0x43E10000, 0x3FAAAAAB } },	/* Ring, The - Terror's Realm (USA) */
		{ "MK-51010  ", NULL,    { 0x1A4D2C, 0x1A4D5C, 0 }, { 0x43F00000, 0x3F400000 } },	/* Rippin' Riders (USA) */
		{ "HDR-0044  ", NULL,    { 0x298A94, 0x298AC4, 0 }, { 0x43F00000, 0x3F400000 } },	/* Roommania #203 (JP) */
		{ "MK-5109250", NULL,    { 0x29E6C4, 0 }, { 0x3F400000 } },		/* Samba De Amigo (PAL) */
		{ "T41301N   ", NULL,    { 0x77A178, 0 }, { 0x3F400000 } },		/* Seventh Cross Evolution (USA) */
		{ "T8104D  58", NULL,    { 0x2C03F8, 0 }, { 0x44558000 } },		/* Shadow Man (PAL) */
		{ "T-8106N   ", NULL,    { 0x2C03F4, 0 }, { 0x3F400000 } },		/* Shadow Man (USA) */
		{ "MK-51048  ", NULL,    { 0x4AA4DC, 0x2B4E30, 0 }, { 0x3F400000, 0x3F400000 } },	/* Seaman (USA) */
		{ "MK-51053  ", NULL,    { 0x5630CC, 0 }, { 0x3F400000 } },		/* Sega GT (USA) */
		{ "MK-5105350", NULL,    { 0x5D613C, 0 }, { 0x3F400000 } },		/* Sega GT (PAL) */
		{ "MK-51096  ", NULL,    { 0x495050, 0 }, { 0x43700000 } },		/* Sega Marine Fishing (USA) */
/* { "MK-51019  ", NULL,    { 0xB83A48, 0 }, { 0x3F400000 } },		// Sega Rally 2 (USA) not working? */
/* { "HDR0010   ", NULL,    { 0xBD9BA0, 0 }, { 0x3F400000 } },		// Sega Rally 2 (JP) not working? */
		{ "HDR-0151  ", NULL,    { 0xAF57DC, 0xAF580C, 0x2122A0, 0 },
				{ 0x43F00000, 0x3F400000, 0x3F400000 } },					/* SGGG – Segagaga (JP) */
		{ "MK-5105950", NULL,    { 0x231EF8, 0x1EF370, 0 }, { 0x43800000, 0x7C1EF400 } },	/* Shenmue (PAL) code 1 reduces clipping */
		{ "MK-51059  ", NULL,    { 0x230250, 0 }, { 0x43800000 }, { 0x43A00000 } },	/* Shenmue (USA) Clipping */
		{ "HDR-0016  ", NULL,    { 0x22E8A0, 0x1EBE70, 0 }, { 0x43800000, 0x7C1EBF00 } },	/* Shenmue (JP) code 1 reduces clipping */
		{ "MK-5118450", NULL,    { 0x31186C, 0 }, { 0x43800000 }, { 0x43A00000 } },	/* Shenmue II (PAL) 01160FF4 0000E100 for black bars in cutscenes */
		/* Shenmue II (PAL) Alternative code without clipping or black bars. Might be demanding on real hardware. */
		/* 02311880 C3A00000, 0227E198 35AA359E, 01160FF4 0000E100 */
		{ "HDR-0164  ", NULL,    { 0x30D67C, 0 }, { 0x43700000 }, { 0x43A00000 } },	/* Shenmue II (JP) Clipping */
		{ "T9505D    ", NULL,    { 0xD1FB14, 0x096A4C, 0 }, { 0x3F400000, 0x3FAAAAAB } },	/* Silent Scope (PAL) Choose 60Hz in game options */
		{ "MK-51052  ", "  E     ",    { 0x502A84, 0 }, { 0x3F400000 } },	/* Skies of Arcadia (PAL) */
		{ "MK-51052  ", " U      ",    { 0x599158, 0 }, { 0x3F400000 } },	/* Skies of Arcadia (USA) */
		{ "T15104D 50", NULL,    { 0x17EF68, 0 }, { 0x43F00010 } },		/* Slave Zero (PAL) Widescreen, but a bit zoomed in */
		{ "MK-5101050", NULL,    { 0x1A50EC, 0x1A511C, 0 }, { 0x43F00000, 0x3F400000 } },	/* Snow Surfers (PAL) */
		{ "MK-5100050", NULL,    { 0x88F528, 0x88F55C, 0 }, { 0x43F00000, 0x3FA66666 } },	/* Sonic Adventure (PAL) */
		{ "MK-51000  ", NULL,    { 0x88F5E8, 0x88F61C, 0 }, { 0x43F00000, 0x3FAAAAAB } },	/* Sonic Adventure (USA) */
		{ "MK-51117  ", NULL,    { 0x28DEF8, 0x28DF28, 0 }, { 0x43F00000, 0x3f400000 } },	/* Sonic Adventure 2 (USA) */
		{ "HDR-0165  ", NULL,    { 0x28DF28, 0x28DEF8, 0 }, { 0x3F400000, 0x43F00000 } },	/* Sonic Adventure 2 (JP) */
		{ "MK-51060  ", NULL,    { 0x112A2C, 0 }, { 0x3F400000 } },		/* Sonic Shuffle (US) */
		{ "MK-5106050", NULL,    { 0x110B4C, 0 }, { 0x3F400000 } },		/* Sonic Shuffle (PAL) */
		{ "T9103M    ", NULL,    { 0x25C714, 0x25C744, 0 }, { 0x43F00000, 0x3F400000 } },	/* Sorcerian - Shichisei Mahou no Shito */
		{ "T1401D  50", NULL,    { 0x2D6138, 0 }, { 0x3F400000 } },		/* Soul Calibur (PAL) */
		{ "T1401N    ", NULL,    { 0x266C28, 0 }, { 0x3F400000 } },		/* Soul Calibur (USA) */
		{ "T36802N   ", "  E     ", { 0x129FA0, 0x12A9BC, 0x1C9FDC, 0 },
				{ 0x3EF55555, 0x3EF55555, 0x000000F0 } },					/* Soul Reaver (PAL) Code 2 is a Render Fix */
		{ "HDR-0190  ", NULL,    { 0x14D3E0, 0 }, { 0x3F400000 } },		/* Space Channel 5 Part 2 (JP) */
		{ "T1216M    ", NULL, { 0x017C38, 0x17F00, 0 }, { 0x3A99999A, 0x3A99999A } }, /* Spawn - In the Demon's Hand v1.003 (JP) */
		{ "T1216N    ", NULL, { 0x017C58, 0x17F20, 0 }, { 0x3A99999A, 0x3A99999A } }, /* Spawn - In the Demon's Hand v1.000 (US) */
		{ "T36816D 50", NULL, { 0x017C78, 0x17F40, 0 }, { 0x3A99999A, 0x3A99999A } }, /* Spawn - In the Demon's Hand v1.000 (EU) */
		/* Star Wars Episode I Racer (USA) */
		/* Code 1-4 removes the black bars on top and bottom in FMV */
		{ "T23001N   ", NULL,    { 0x17AE20, 0x29A96C, 0x29A98C, 0x29A9AC, 0x29A9CC, 0 },
				{ 0x3F400000, 0x42900000, 0x42900000, 0x43CE0000, 0x43CE0000 } },
		{ "T40206N   ", NULL,    { 0x43296C, 0 }, { 0x3F400000, 0 } },	/* Super Magnetic Neo (US) */
		{ "T40206D 50", NULL,    { 0x43E34C, 0 }, { 0x3F400000, 0 } },	/* Super Magnetic Neo (EU) */
/* { "T7014D  50", NULL,    { 0xE2B234, 0 }, { 0x3F800000 } },		// Super Runabout (PAL) doesn't work? */
		{ "T40216N   ", NULL,    { 0x45CE54, 0 }, { 0x3F400000 } },		/* Surf Rocket Racers (US) */
		{ "T17721D 50", NULL,    { 0x45CED4, 0 }, { 0x3F400000 } },		/* Surf Rocket Racers (PAL) alt: 021EBF40 3F400000 */
		{ "T17703D 50", NULL,    { 0xCD8950, 0 }, { 0x3F111111 } },		/* Suzuki Alstare Extreme Racing */
		{ "T36807D 05", NULL,    { 0x140F74, 0x140FA4, 0 }, { 0x43FA0000, 0x3F400000 } },	/* Sword of Bersek (PAL) */
		{ "T-36805N  ", NULL,    { 0x13F1C4, 0x13F194, 0 }, { 0x3F400000, 0x43F00000 } },	/* Sword of Bersek (USA) */
		{ "MK-51186  ", NULL,    { 0x4A19B0, 0 }, { 0x43700000 } },		/* Tennis 2K2 (USA) */
		{ "T15123N   ", NULL,    { 0x29B7BC, 0 }, { 0x3F400000 } },		/* Test Drive Le Mans (USA) doesn't work? */
		{ "T20801M   ", NULL,    { 0x1AAC80, 0x1AACB0, 0 }, { 0x43F00000, 0x3F400000 } },	/* Tetris 4D (JP) */
		{ "MK-5101153", NULL,    { 0x14EFA8, 0x14EFD8, 0 }, { 0x43F00000, 0x3F400000 } },	/* Timestalkers (PAL) */
		{ "T7009D50  ", NULL,    { 0x39173C, 0 }, { 0x3F400000 } },		/* Tech Romancer (PAL) */
		{ "T35402M   ", NULL,    { 0x315370, 0x3153A0, 0 }, { 0x43F00000, 0x3F400000 } },	/* Tokyo Bus Guide (JP) doesn't work? */
		{ "T40201D 50", NULL,    { 0x1D9F10, 0 }, { 0x3F400000 } },		/* Tokyo Highway Challenge (PAL) */
		{ "T40210D 50", NULL,    { 0x21E4F8, 0 }, { 0x43700000 } },		/* Tokyo Highway Challenge 2 (PAL) */
		{ "T40202N   ", NULL,    { 0x1D9EB0, 0 }, { 0x3F400000 } },		/* Tokyo Xtreme Racer (USA) */
		{ "T40211N   ", NULL,    { 0x21DEF8, 0 }, { 0x43700000 } },		/* Tokyo Xtreme Racer 2 (USA) */
/* { "T36804D05 ", NULL,    { 0xB75E28, 0 }, { 0x3EC00000 } },		// Tomb Raider: The Last Revelation (UK) (PAL) clipping, use hex patch instead */
		{ "T13008D 05", NULL,    { 0x1D7C20, 0 }, { 0x3FA66666 } },		/* Tony Hawk's Pro Skater 2 (PAL) */
		{ "T13006N   ", NULL,    { 0x1D77A0, 0 }, { 0x3FA66666 } },		/* Tony Hawk's Pro Skater 2 (USA) */
		{ "MK-5102050", NULL,    { 0x0D592C, 0 }, { 0x3FD00000 } },		/* Toy Commander (PAL) */
		{ "xxxxxxxxxx", NULL,    { 0x469FCC, 0x469FFC, 0 }, { 0x44700000, 0x3F400000 } },	/* Toyota Doricatch Series: Land Cruiser 100/Cygnus */
		{ "MK-5109505", NULL,    { 0x1B2718, 0 }, { 0x3F400000 } },		/* UEFA Dream Soccer (PAL) */
		{ "T40203D 50", NULL,    { 0x1D74E8, 0x1D7518, 0 }, { 0x43F00000, 0x3F400000 } },	/* Ultimate Fighting Championship (PAL) */
		{ "T40204N   ", NULL,    { 0x1A9684, 0 }, { 0x3F400000 } },		/* Ultimate Fighting Championship (USA) problems with cam in game */
		{ "T-8110D-50", NULL,    { 0x0C6B90, 0x0C6B94, 0 }, { 0x43F00000, 0x43870000 } },	/* Vanishing Point (PAL) */
/* { "MK-5109450", NULL,    { 0x244134, 0x7A73B8, 0x7830D8, 0x7A74B8, 0x783138, 0x7A85A0, 0x6C7928, 0x6C7930, 0x6C7948, 0x6C7950, 0 }, */
/* crash				{ 0x43F00000, 0x3F9C932E, 0x3F9C932E, 0x3F9C932E, 0x3F9C932E, 0x3F400000, 0, 0, 0, 0 } },	// Virtua Athlete 2K (PAL) */
		{ "MK-51001  ", NULL,    { 0x19D518, 0 }, { 0x43700000 } },		/* Virtua Fighter 3 TB (USA) */
		{ "MK-5100150", NULL,    { 0x19D718, 0 }, { 0x43700000 } },		/* Virtua Fighter 3 TB (PAL) */
		{ "HDR-0002  ", NULL,    { 0x199FB0, 0 }, { 0x43700000 } },		/* Virtua Fighter 3 TB (JP) */
		{ "MK-5105450", NULL,    { 0x456378, 0 }, { 0x43700000 } },		/* Virtua Tennis (v1.001) (PAL) */
		{ "MK-51054  ", NULL,    { 0x450A90, 0 }, { 0x43700000 } },		/* Virtua Tennis (USA) */
		{ "MK-5118650", NULL,    { 0x4A4A20, 0 }, { 0x43700000 } },		/* Virtua Tennis 2 (PAL) */
/* { "T0000     ", NULL,    { 0x3A514C, 0x3A6170, 0 }, { 0x3F400000, 0x00000356 } },	// Volgarr the Viking. Not working */
		{ "xxxxxxxxxx", NULL,    { 0x20BB68, 0x1ACBD0, 0x1B9ADC, 0 },	/* Code 1 reduces clipping.	Code 2 fixes the clock. */
				{ 0x43700000, 0x7C1ACC60, 0x3F400000 } },					/* What's Shenmue (JP) */
		{ "T40504D 50", NULL,    { 0x75281C, 0 }, { 0x3F400000 } },		/* Wetrix+ (PAL) not working? */
		{ "MK-51152  ", NULL,    { 0x014E90, 0 }, { 0x43700000 } },		/* World Series Baseball 2K2 (USA) */
		{ "T20401M   ", NULL,    { 0x323CB0, 0x1ACBD0, 0x1B9ADC, 0 },	/* Code 1 reduces clipping. Code 2 fixes the HUD. */
				{ 0x43700000, 0x1ACC60, 0x3F400000 } },						/* Zero Gunner 2 (JP) */
		{ "MK-5103850", NULL,    { 0x9484E8, 0 }, { 0x43700000 } },		/* Zombie Revenge (PAL) */
		{ "MK-51038  ", NULL,    { 0x948058, 0 }, { 0x43700000 } },		/* Zombie Revenge (USA) */
		{ "HDR-0026  ", NULL,    { 0x948B18, 0 }, { 0x43700000 } },		/* Zombie Revenge (JP) */
		{ "T43301M   ", NULL,    { 0x4B0218, 0 }, { 0x3F400000 } },		/* Zusar Vasar (JP) */

		{ NULL },
};
static const Cheat naomi_widescreen_cheats[] =
{
		{ "KNIGHTS OF VALOUR  THE 7 SPIRITS", NULL, { 0x475B70, 0x475B40, 0 }, { 0x3F400000, 0x43F00000 } },
		{ "Dolphin Blue", NULL, { 0x3F2E2C, 0x3F2190, 0x3F2E6C, 0x3F215C, 0 },
				{ 0x43B90000, 0x3FAA9FBE, 0x43B90000, 0x43F00000 } },
		{ "FASTER THAN SPEED", NULL, { 0x3488E0, 0 }, { 0x3F400000 } }, /* ftspeed */
		{ "METAL SLUG 6", NULL, { 0xE93478, 0xE9347C, 0 }, { 0x3F400000, 0x3F8872B0 } },
		{ "TOY FIGHTER", NULL, { 0x133E58, 0 }, { 0x43700000 } },
		{ "LUPIN THE THIRD  -THE SHOOTING-", NULL, { 0x045490, 0 }, { 0x3F400000 } },
		{ "VF4 FINAL TUNED JAPAN", NULL, { 0x02B834, 0x0AFB90, 0 }, { 0x3FE38E39, 0x3FE38E39 } },
		{ "DYNAMITE DEKA EX", NULL, { 0x0E3598, 0x0C8E84, 0 }, { 0x3FE38E38, 0x3FE38E38 } },
		{ "DEAD OR ALIVE 2", "doa2m", { 0x085B5C, 0x086AE8, 0 }, { 0x3FE38E39, 0x3FE38E39 } },
		{ "SLASHOUT JAPAN VERSION", NULL, { 0x3DDBE4, 0 }, { 0x43CFDC86 } },
		{ "SPIKERS BATTLE JAPAN VERSION", NULL, { 0x3626C4, 0 }, { 0x43A551B0 } },
		{ "  18WHEELER", "18wheelr", { 0x5C64A8, 0 }, { 0x43700000 } },
		{ "BEACH SPIKERS JAPAN", NULL, { 0x065A7C, 0 }, { 0x44558000 } },
		{ "GUN SPIKE", NULL, { 0x21A94, 0 }, { 0x3FE38E39 } },
		{ "INITIAL D", "initd", { 0x155434, 0 }, { 0x3FE38E39 } },
		{ "INITIAL D", "initdexp", { 0x159674, 0 }, { 0x3FE38E39 } },
		{ "INITIAL D", "initdexpo", { 0x159634, 0 }, { 0x3FE38E39 } },
		{ "INITIAL D", "initdo", { 0x14F5F4, 0 }, { 0x3FE38E39 } },
		{ "INITIAL D Ver.2", "initdv2e", { 0x1B4F74, 0 }, { 0x3FE38E39 } },
		{ "INITIAL D Ver.2", "initdv2j", { 0x1B4F34, 0 }, { 0x3FE38E39 } },
		{ "INITIAL D Ver.2", "initdv2jo", { 0x1AD1F4, 0 }, { 0x3FE38E39 } },
		{ "INITIAL D Ver.3", "initdv3e", { 0x1D0B34, 0 }, { 0x3FE38E39 } },
		{ "INITIAL D Ver.3", "initdv3j", { 0x1D7C74, 0 }, { 0x3FE38E39 } },
		{ "INITIAL D Ver.3", "initdv3jb", { 0x1D7774, 0 }, { 0x3FE38E39 } },
		{ "AIRLINE PILOTS IN JAPAN", "alpilotj", { 0x1D62550, 0 }, { 0x43700000 } },
		{ "MONKEY BALL JAPAN VERSION", NULL, { 0x345B4, 0x45244, 0x454CC, 0 }, { 0x3FE38E39, 0x3FE38E39, 0x3FE38E39 } },
		{ "ZOMBIE REVENGE IN JAPAN", "zombrvn", { 0x7A4808, 0 }, { 0x43700000 } },
		{ "ZOMBIE REVENGE IN JAPAN", "zombrvno", { 0x7A2E50, 0 }, { 0x43700000 } },
		{ " BIOHAZARD  GUN SURVIVOR2", "gunsur2", { 0x42EE80, 0 }, { 0x3F400000 } },
		{ " BIOHAZARD  GUN SURVIVOR2", "gunsur2j", { 0x42EBE0, 0 }, { 0x3F400000 } },
		{ "SPAWN JAPAN", NULL, { 0x02DEF8, 0x02E1C0, 0 }, { 0x3A99999A, 0x3A99999A } },
		/* gun coords problem with these 2 cheats */
		/* { " CONFIDENTIAL MISSION ---------", NULL, { 0x24F798 }, { 0x43700000 } }, */
		/* { "hotd2", NULL, { 0x9C2AD8 }, { 0x43700000 } }, */
		{ " JAMBO SAFARI ------------", NULL, { 0x2B1DE0, 0 }, { 0x3FE38E39 } },
		{ "SOUL SURFER IN JAPAN", NULL, { 0x8962C8, 0 }, { 0x3FE38E39 } },
		{ "LOVE AND BERRY 3 EXP VER1.002", NULL, { 0x3C16E4, 0 }, { 0x3F400000 } },	/* loveber3 */
		{ "LOVE AND BERRY USA VER2.000", NULL, { 0x4E92D0, 0 }, { 0x3F400000 } },	/* lovebery */

		{ NULL },
};
/* Each game's entries one after the other. */
static const GameFix game_fixes[] =
{
		/* Extreme Hunting 2: no network board check, and the boot code put back as it should be */
		{ "Fixed BOOT strapper", 32, 0x067B04, 1, true, 0 },
		{ "Fixed BOOT strapper", 16, 0x09ACC8, 0x0009, false, 0 },
		{ "Fixed BOOT strapper", 32, 0x010000, 0x9302D202, true, 0x8C9378C0 },
		/* The King of Route 66: a drive board error is not stopped at (rts, nop) */
		{ "THE KING OF ROUTE66", 32, 0x023EE0, 0x0009000B, false, 0 },
		/* Dave Mirra Freestyle BMX: the main loop's time (33 ms in the US, 40 ms in Europe) */
		{ "T-8120N", 32, 0x30B8CC, 0x42040000, false, 0 },
		{ "T8120D  50", 32, 0x3011CC, 0x42200000, false, 0 },
		/* Samurai Shodown VI: depth shifted by 8 bits instead of 10 (nop) */
		{ "SAMURAI SPIRITS 6", 16, 0x03E602, 0x0009, false, 0 },
		{ "T0002M", 16, 0x03E602, 0x0009, false, 0 },
		/* Fur Fighters: logging on, for the time it takes, without which the game hangs */
		{ "T-8107N", 32, 0x314248, 1, false, 0 },
		{ "T-8113D-50", 32, 0x314228, 1, false, 0 },
		/* Dricas authentication, which the servers that are left do not do,
		 * answered with success (rts, mov #0,r0) */
		{ "T6807M", 32, 0x04B7A0, 0xE000000B, true, 0x2FD62FE6 },	/* Aero Dancing i: modem */
		{ "T6807M", 32, 0x04AF5C, 0xE000000B, true, 0x2FD62FE6 },	/* broadband adapter */
		{ "T6807M", 32, 0x020860, 0xE000000B, true, 0x4F222FE6 },	/* IP check */
		{ "T6809M", 32, 0x04B940, 0xE000000B, true, 0x2FD62FE6 },	/* Aero Dancing i - Jikai Saku Made Matemasen: modem */
		{ "T6809M", 32, 0x04F848, 0xE000000B, true, 0x2FD62FE6 },	/* broadband adapter */
		{ "T6809M", 32, 0x020980, 0xE000000B, true, 0x4F222FE6 },	/* IP check */
		{ "T6805M", 32, 0x03ED10, 0xE000000B, true, 0x2FD62FE6 },	/* Aero Dancing F */
		{ "HDR-0106", 32, 0x03AD30, 0xE000000B, true, 0x2FD62FE6 },	/* Daytona USA (JP) */
		{ "HDR-0073", 32, 0x0A56F8, 0xE000000B, true, 0x2FD62FE6 },	/* Sega Tetris */
		{ "T44501M", 32, 0x13F150, 0xE000000B, true, 0x2FD62FE6 },	/* Golf Shiyou Yo 2 */
		{ "T43903M", 32, 0x800524, 0xE000000B, true, 0x2FD62FE6 },	/* Culdcept II */
		{ "HDR-0118", 32, 0x139F54, 0xE000000B, true, 0x2FD62FE6 },	/* Outtrigger (JP) */
		{ "HDR-0100", 32, 0x0BA980, 0xE000000B, true, 0x2FD62FE6 },	/* F355 Challenge (JP) */
		{ "HDR-0100", 32, 0x0A2D3C, 0xED048B00, true, 0xED048900 },	/* and its check of it (bt -> bf) */
		{ "HDR-0006", 32, 0x0D5FBC, 0xE000000B, true, 0x2FD62FE6 },	/* Nettou Golf */
		/* Ooga Booga: no check of the network's state */
		{ "MK-51140", 16, 0x085D2C, 0x3330, true, 0x3630 },

		{ NULL },
};

/* The game's widescreen cheat, or NULL, and what the game had where it
 * writes, as last seen there: put back when the cheat is turned off */
static const Cheat *ws_cheat;
static u32 ws_original[16];
static u32 ws_have_original;
/* The game's fixes, one after the other in game_fixes[] */
static const GameFix *fixes;
static u32 fix_count;

/* Turned off: what the cheat wrote is taken out again. The cheat writes
 * the same words every frame, most of them the game's own constants, which
 * the game never writes again - left there, the picture stayed as the cheat
 * made it until the game was restarted. Turned on: cheats_reset() finds the
 * game's cheat as it does when the game is loaded. */
bool cheats_change(void)
{
	if (ws_cheat != NULL && !settings.rend.WidescreenGameHacks)
	{
		size_t i;
		for (i = 0; i < ARRAY_SIZE(ws_cheat->addresses) && ws_cheat->addresses[i] != 0; i++)
		{
			const u32 addr = 0x8C000000 + ws_cheat->addresses[i];

			/* (not if the game has written there since the cheat last did) */
			if ((ws_have_original & (1u << i)) && ReadMem32_nommu(addr) == ws_cheat->values[i])
				WriteMem32_nommu(addr, ws_original[i]);
		}
	}
	else if (ws_cheat != NULL)
		return true;
	return cheats_reset();
}

bool cheats_reset(void)
{
	/* the product number as it is in the IP.BIN, and the fixes' name for
	 * the game: it without the spaces after it, or an arcade game's name */
	char game_id[sizeof(ip_meta.product_number) + 1];
	char fix_id[64];
	size_t fix_len;
	int i;

	ws_cheat = NULL;
	ws_have_original = 0;

	memcpy(game_id, ip_meta.product_number, sizeof(ip_meta.product_number));
	game_id[sizeof(ip_meta.product_number)] = '\0';
	/* (all ten bytes of the product number, any zero among them too: such a
	 * number is not one of the table's) */
	if (settings.System == DC_PLATFORM_DREAMCAST)
	{
		fix_len = sizeof(ip_meta.product_number);
		memcpy(fix_id, game_id, fix_len);
	}
	else
	{
		fix_len = strlen(naomi_game_id);
		if (fix_len > sizeof(fix_id) - 1)
			fix_len = sizeof(fix_id) - 1;
		memcpy(fix_id, naomi_game_id, fix_len);
	}
	while (fix_len > 0 && fix_id[fix_len - 1] == ' ')
		fix_len--;
	fix_id[fix_len] = '\0';
	fixes = NULL;
	fix_count = 0;
	for (i = 0; game_fixes[i].game_id != NULL; i++)
	{
		if (strlen(game_fixes[i].game_id) != fix_len
				|| memcmp(fix_id, game_fixes[i].game_id, fix_len) != 0)
			continue;
		if (fixes == NULL)
			fixes = &game_fixes[i];
		fix_count++;
	}
	if (fixes != NULL)
		NOTICE_LOG(COMMON, "Applying %u fixes to game %s\n", fix_count, fix_id);

	if (!settings.rend.WidescreenGameHacks)
		return false;
	if (settings.System == DC_PLATFORM_DREAMCAST)
	{
		/* (not on the fourth disc of Shenmue II, whose cheat does not fit
		 * it) */
		if (((fix_len == 8 && !memcmp(fix_id, "HDR-0164", 8))
					|| (fix_len == 10 && !memcmp(fix_id, "MK-5118450", 10)))
				&& !memcmp(ip_meta.disk_num, "4/4  ", 5))
			return false;
		for (i = 0; widescreen_cheats[i].game_id != NULL; i++)
		{
			if (!strncmp(game_id, widescreen_cheats[i].game_id, sizeof(ip_meta.product_number))
					&& (widescreen_cheats[i].area_or_version == NULL
							|| !strncmp(ip_meta.area_symbols, widescreen_cheats[i].area_or_version, sizeof(ip_meta.area_symbols))
							|| !strncmp(ip_meta.product_version, widescreen_cheats[i].area_or_version, sizeof(ip_meta.product_version))
							|| !strncmp(ip_meta.software_name, widescreen_cheats[i].area_or_version, sizeof(ip_meta.software_name))))
			{
				ws_cheat = &widescreen_cheats[i];
				NOTICE_LOG(COMMON, "Applying widescreen hack to game %s\n", game_id);
				break;
			}
		}
	}
	else
	{
		/* The set's name, which tells apart versions with the same name
		 * inside: the file's, as the set is asked for by it. */
		char set_name[128];

		memset(set_name, 0, sizeof(set_name));
		if (game_data != NULL)
		{
			strncpy(set_name, path_basename(game_data), sizeof(set_name) - 1);
			path_remove_extension(set_name);
		}
		for (i = 0; naomi_widescreen_cheats[i].game_id != NULL; i++)
		{
			if (!strcmp(naomi_game_id, naomi_widescreen_cheats[i].game_id)
					&& (naomi_widescreen_cheats[i].area_or_version == NULL
							|| !strcmp(set_name, naomi_widescreen_cheats[i].area_or_version)))
			{
				ws_cheat = &naomi_widescreen_cheats[i];
				NOTICE_LOG(COMMON, "Applying widescreen hack to game %s\n", naomi_game_id);
				break;
			}
		}
	}
	if (ws_cheat == NULL)
		return false;
	/* (a word that is not in this machine's memory: no cheat) */
	for (i = 0; i < (int)ARRAY_SIZE(ws_cheat->addresses) && ws_cheat->addresses[i] != 0; i++)
		if (ws_cheat->addresses[i] > RAM_SIZE - 4)
		{
			WARN_LOG(COMMON, "Widescreen hack of game %s is outside memory\n", game_id);
			ws_cheat = NULL;
			return false;
		}

	return true;
}

void cheats_apply(void)
{
	u32 f;
	size_t i;

	for (f = 0; f < fix_count; f++)
	{
		const GameFix *fix = &fixes[f];
		/* (the table's addresses are all well inside the smallest memory) */
		const u32 addr = 0x8C000000 + fix->address;
		const u32 now = fix->bits == 32 ? ReadMem32_nommu(addr)
				: fix->bits == 16 ? ReadMem16_nommu(addr) : ReadMem8_nommu(addr);
		if (now == fix->value || (fix->only_if && now != fix->if_value))
			continue;
		if (fix->bits == 32)
			WriteMem32_nommu(addr, fix->value);
		else if (fix->bits == 16)
			WriteMem16_nommu(addr, (u16)fix->value);
		else
			WriteMem8_nommu(addr, (u8)fix->value);
	}
	if (ws_cheat == NULL)
		return;
	for (i = 0; i < ARRAY_SIZE(ws_cheat->addresses) && ws_cheat->addresses[i] != 0; i++)
	{
		const u32 addr = 0x8C000000 + ws_cheat->addresses[i];
		const u32 now  = ReadMem32_nommu(addr);
		/* (only over the value it is meant for, where the cheat says which) */
		if (ws_cheat->orig_values[i] != 0 && now != ws_cheat->orig_values[i]
				&& now != ws_cheat->values[i])
			continue;

		/* Whatever is there that is not the cheat's is the game's: the
		 * program as it was loaded, or a value the game has just set.
		 * It is what goes back if the cheat is turned off (cheats_change()). */
		if (now != ws_cheat->values[i])
		{
			ws_original[i]    = now;
			ws_have_original |= 1u << i;
			WriteMem32_nommu(addr, ws_cheat->values[i]);
		}
	}
}
