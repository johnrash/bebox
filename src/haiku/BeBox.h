/*
 * BeBox emulator for Haiku - shared definitions.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#ifndef BEBOX_APP_DEFS_H
#define BEBOX_APP_DEFS_H

#include <SupportDefs.h>

#define kAppSignature "application/x-vnd.BeBox-Emulator"
#define kAppName "BeBox"

enum {
	kMsgPowerOn = 'pwon',
	kMsgPowerOff = 'pwof',
	kMsgReset = 'rset',
	kMsgInterruptSwitch = 'intr',
	kMsgCtrlAltDelete = 'cadl',
	kMsgSwitchProfile = 'swpf',
	kMsgNewProfile = 'nwpf',
	kMsgRenameProfile = 'rnpf',
	kMsgDeleteProfile = 'dlpf',
	kMsgPause = 'paus',
	kMsgPreferences = 'pref',
	kMsgPreferencesChanged = 'prch',
	kMsgFrame = 'fram',
	kMsgScale = 'scal',
	kMsgShowLeds = 'tled',
	kMsgLedStyle = 'lsty',
	kMsgInsertFloppy = 'insf',
	kMsgFloppyChosen = 'flch',
	kMsgEjectFloppy = 'ejcf',
	kMsgCaptureChanged = 'capt',
	kMsgBootWithMenu = 'btmn',
	kMsgBootWithDebug = 'btdb',
	kMsgScreenshot = 'shot',
	kMsgSpeedLimit = 'sped',
	kMsgTypeKeys = 'type',
	kMsgAbout = 'abou',
	kMsgNewDiskCreated = 'ndcr',
	kMsgResetFlash = 'rsfl',
	kMsgClearNvram = 'clnv',
	kMsgSerialConsole = 'scon'
};

enum LedStyle {
	kLedStyleAuthentic = 0,		// lime green load and power, amber disk
	kLedStyleRedGreen = 1		// green load with red top, red disk
};

#endif	// BEBOX_APP_DEFS_H
