/*
 * BeBox emulator for Haiku - the machine window.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Menu bar, the emulated screen between the two front panel LED columns,
 * and a status line. While the window is active all keys go to the
 * emulated keyboard except the host key (right Control), which toggles
 * the mouse capture.
 *
 * Scripting (hey BeBox ... of Window 0):
 *   get/set Running (bool), get Speed, Mips, EmulatedTime (double),
 *   get LedLevels (two int32: left, right, 0-15), get DiskLed (bool),
 *   get SerialLog [1-4] (console output), get/set Floppy (image, "" ejects)
 *   get/set Profile (the machine profile; set switches, restarting a
 *   running machine), get Profiles (all names)
 *   pwon, pwof, rset: power on, off, reset
 *   type with text=...: types on the emulated keyboard ("\n" is Enter)
 *   shot [with path=...]: saves a PNG of the screen (replies "path")
 * Replies come after the action.
 */
#ifndef BEBOX_MACHINE_WINDOW_H
#define BEBOX_MACHINE_WINDOW_H

#include "HaikuAttributes.h"
#include "HaikuAudio.h"
#include "Settings.h"
#include "core/host/EmulatorThread.h"
#include "core/host/NetworkBackend.h"
#include "core/host/SerialBackend.h"

#include <Messenger.h>
#include <String.h>

#include <memory>
#include <vector>

#include <Window.h>

class BFilePanel;
class BJoystick;
class BMessageRunner;
class BMenu;
class ConsoleSerial;
class BMenuItem;
class BStringView;
class LedColumnView;
class MediaStatusView;
class ScreenView;

class MachineWindow : public BWindow {
public:
								MachineWindow(Settings& settings);
	virtual						~MachineWindow();

			bool				PowerOn();
			void				PowerOff();
			// Restarts a running machine with the profile. 'create' makes
			// a missing profile as a copy of the current one.
			bool				SwitchProfile(const char* name, bool create);

	virtual	void				DispatchMessage(BMessage* message,
									BHandler* handler);
	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();
	virtual	void				WindowActivated(bool active);
	virtual	void				FrameMoved(BPoint origin);
	virtual	void				FrameResized(float width, float height);

	virtual	BHandler*			ResolveSpecifier(BMessage* message,
									int32 index, BMessage* specifier,
									int32 what, const char* property);
	virtual	status_t			GetSupportedSuites(BMessage* data);

private:
			BMenu*				_BuildMachineMenu();
			void				_BuildProfileMenu();
			void				_NewProfile(const char* name);
			void				_RenameProfile(const char* name);
			void				_DeleteProfile();
			void				_ApplySettings();
			void				_ClosePreferences();
			BMenu*				_BuildMediaMenu();
			BMenu*				_BuildViewMenu();
			bool				_HandleKey(BMessage* message);
			int32				_HostKeyIndex() const;
			void				_ModifiersChanged(uint32 old, uint32 now);
			void				_SendKey(uint32 haikuKey, bool down);
			void				_ReleaseAllKeys();
			void				_UpdateStatus();
			bool				_ShowLeds() const;
			bool				_UpdateLeds();
			void				_UpdateMenus();
			void				_FitWindow();
			void				_HoldKeyDuringBoot(uint32 set2, double seconds);
			void				_ShowConsole(int port);
			BString				_SaveScreenshot(const char* path = nullptr);
			bool				_HandleScripting(BMessage* message);
			void				_InsertFloppy(const char* path);
			void				_EjectFloppy();
			void				_PrepareSharedFolders(
									bebox::MachineConfig& config);
			void				_SyncSharedFolders(bool interactive);
			void				_OpenJoysticks();
			void				_CloseJoysticks();
			void				_PollJoysticks();
			void				_TypeText(const char* text);
			void				_PressKeyCombination(
									const std::vector<const char*>& keys);

			Settings&			fSettings;
			// declared first: outlive the emulation thread using them
			HaikuAudio			fAudio;
			ConsoleSerial*		fConsoles[4];
			std::vector<std::unique_ptr<bebox::SerialBackend> >
								fSerialBackends;
			BMessenger			fConsoleWindows[4];
			std::unique_ptr<bebox::NetworkBackend> fNetwork;
			HaikuAttributes		fHostAttributes;
			std::vector<std::unique_ptr<bebox::SharedFolder> >
								fSharedFolders;
			BMenu*				fConsoleMenu;
			bebox::EmulatorThread fEmulator;
			ScreenView*			fScreen;
			LedColumnView*		fLeftLeds;
			LedColumnView*		fRightLeds;
			BStringView*		fStatus;
			MediaStatusView*	fMedia;
			BMenuItem*			fPowerItem;
			BMenuItem*			fPauseItem;
			BMenuItem*			fCtrlAltDeleteItem;
			BMenu*				fProfileMenu;
			BMessenger			fPrefsWindow;
			BMenuItem*			fScaleItems[3];
			BMenuItem*			fShowLedsItem;
			BMenuItem*			fEjectFloppyItem;
			BFilePanel*			fFloppyPanel;
			bool				fKeysDown[128];
			uint32				fModifiers;
			bool				fPowered;
			BJoystick*			fJoysticks[4];
			BMessageRunner*		fJoystickRunner;
};

#endif	// BEBOX_MACHINE_WINDOW_H
