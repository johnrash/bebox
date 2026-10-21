/*
 * BeBox emulator for Haiku - persistent settings and machine profiles.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * In ~/config/settings/BeBox: "settings" (a flattened BMessage) holds what
 * all profiles share (keyboard and mouse, the LED look, the window) and
 * the current profile's name; each profile is a folder in Profiles/ with
 * the machine's own settings, boot ROM flash and NVRAM.
 */
#ifndef BEBOX_SETTINGS_H
#define BEBOX_SETTINGS_H

#include "core/board/MachineConfig.h"

#include <Rect.h>
#include <String.h>
#include <StringList.h>

class BMessage;

// Keys that release the mouse (and are not passed to the BeBox).
enum HostKey {
	kHostKeyRightControl = 0,
	kHostKeyRightCommand = 1,
	kHostKeyRightOption = 2,
	kHostKeyMenu = 3
};

class Settings {
public:
								Settings();

			// The shared settings and the current profile. Settings from
			// before profiles become the profile "Default".
			status_t			Load();
			status_t			Save() const;

	static	BString				Directory();
	static	BString				ProfilesDirectory();
			BString				ProfileDirectory() const;
			// Fills the paths derived from the settings directory.
			bebox::MachineConfig MachineConfigForRun() const;
			BString				FlashPath() const;
			BString				NvramPath() const;

	// Profiles
	static	void				GetProfiles(BStringList& names);
	static	bool				ProfileExists(const char* name);
			// Why a name cannot be used for a new profile ("" if it can).
	static	BString				CheckProfileName(const char* name);
			// Saves the current profile and loads 'name'.
			status_t			SwitchProfile(const char* name);
			// A copy of the current profile (its settings, flash and
			// NVRAM) named 'name', which becomes the current one. Files
			// already in the new profile's folder are kept.
			status_t			CopyProfile(const char* name);
			status_t			RenameProfile(const char* name);
			// Deletes another profile.
	static	status_t			DeleteProfile(const char* name);

			// A device kind set without an image would stop the machine
			// from starting: such devices become "none" (a CD-ROM drive
			// stays, empty).
	static	void				DropEmptyDevices(bebox::MachineConfig& machine);

			BString				profile;

			// The profile's settings
			bebox::MachineConfig machine;
			BString				romDirectory;
			// Main boot ROM: a ROM update image (bebox_bootmain.image of
			// a BeOS CD); empty = bootmain.rom of the ROM directory.
			BString				bootRomImage;
			int32				speedLimit;		// percent, 0 = unlimited
			bool				soundEnabled;
			int32				soundVolume;	// percent
			// Serial port destinations: "" (not connected), "console",
			// "tcp-listen:[ADDR:]PORT", "tcp-connect:HOST:PORT",
			// "host:DEVICE", "file:PATH".
			BString				serial[4];
			// Host joysticks (BJoystick device names) on the BeBox's four
			// game port sticks; "" = none.
			BString				joystick[4];
			// Ethernet (NE2000, ISA) on the user mode network: card
			// resources, MAC address ("00:00:e8:..."), host port forwards
			// ("tcp:2121:21,...").
			bool				ethernet;
			int32				ethernetPort;
			int32				ethernetIrq;
			BString				macAddress;
			BString				portForwards;

			// Shared by all profiles
			int32				scale;			// 1, 2, 0 = fit window
			bool				showLeds;
			int32				ledStyle;
			int32				hostKey;		// HostKey: releases the mouse
			int32				mouseSpeed;		// percent
			bool				swapMouseButtons;
			bool				captureOnClick;
			BRect				windowFrame;

private:
			void				_ResetProfile();
			void				_ReadProfile(const BMessage& archive);
			void				_WriteProfile(BMessage& archive) const;
			status_t			_LoadProfile();
			status_t			_SaveProfile() const;
			void				_Migrate(const BMessage& archive);
};

#endif	// BEBOX_SETTINGS_H
