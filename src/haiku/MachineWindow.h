/*
 * BeBox emulator for Haiku - the machine window.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Menu bar, the emulated screen between the two front panel LED columns,
 * and a status line. While the window is active all keys go to the
 * emulated keyboard except the host key (right Control), which toggles
 * the mouse capture; the host key with F toggles full screen (the screen
 * and the LEDs only).
 *
 * Scripting (hey BeBox ... of Window 0):
 *   get/set Running (bool), get Speed, Mips, EmulatedTime (double),
 *   get LedLevels (two int32: left, right, 0-15), get DiskLed (bool),
 *   get SerialLog [1-4] (console output), get/set Floppy (image, "" ejects)
 *   get/set CdRom [SCSI ID] (the disc image of that CD-ROM drive or of the
 *   first one, "" ejects)
 *   get/set Profile (the machine profile; set switches, restarting a
 *   running machine), get Profiles (all names)
 *   get/set FullScreen (bool)
 *   pwon, pwof, rset: power on, off, reset
 *   type with text=...: types on the emulated keyboard ("\n" is Enter)
 *   shot [with path=...]: saves a PNG of the screen (replies "path")
 * Replies come after the action.
 */
#ifndef BEBOX_MACHINE_WINDOW_H
#define BEBOX_MACHINE_WINDOW_H

#include "HaikuAttributes.h"
#include "HaikuAudio.h"
#include "JobQueue.h"
#include "Settings.h"
#include "core/Log.h"
#include "core/host/EmulatorThread.h"
#include "core/host/NetworkBackend.h"
#include "core/host/SerialBackend.h"

#include <Messenger.h>
#include <String.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <Window.h>

namespace bebox {
class CdImage;
}

class BFilePanel;
class BGroupView;
class BJoystick;
class BMessageRunner;
class BMenu;
class BMenuBar;
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

			// With shared folders the machine starts when they are
			// copied (on fFolderJobs); true while it is starting.
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
	virtual	void				ScreenChanged(BRect frame, color_space mode);

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
			void				_TrackHostKey(BMessage* message);
			bool				_HandleHostCombination(BMessage* message);
			void				_SetFullScreen(bool fullScreen);
			int32				_HostKeyIndex() const;
			void				_ModifiersChanged(uint32 old, uint32 now);
			void				_SendKey(uint32 haikuKey, bool down);
			void				_ReleaseAllKeys();
			void				_UpdateStatus();
			// The core's log: storage and persistence errors go to the
			// window (_ShowProblem()), besides the standard error.
	static	void				_LogSink(bebox::LogCategory category,
									bebox::LogLevel level,
									const char* message, void* cookie);
			void				_ShowProblem(const char* text);
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
			// The SCSI IDs of the CD-ROM drives.
			std::vector<int>	_CdDrives() const;
			void				_UpdateCdItems();
			void				_ShowCdPanel(int id);
			// Puts the disc in the drive (the old one comes out first); an
			// image that does not open is reported in 'errorText', or in
			// an alert.
			bool				_InsertCd(int id, const char* path,
									BString* errorText = nullptr);
			void				_EjectCd(int id);
			void				_ChangeCd(int id,
									std::unique_ptr<bebox::CdImage> image);
			void				_MediaChanged();
			bool				_StartMachine(
									const bebox::MachineConfig& config);
			void				_PrepareSharedFolders(
									const bebox::MachineConfig& config);
			void				_SharedFoldersReady(BMessage* message);
			void				_SyncSharedFolders(bool interactive);
			void				_ReplyToStart(status_t status);
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
			// the shared folders' copies; destroyed first: finishes them
			// (they use fHostAttributes)
			JobQueue			fFolderJobs;
			struct FolderStart;
			std::shared_ptr<FolderStart> fFolderStart;
			int32				fFolderStarts;
			int32				fFolderSyncs;	// copies back running
			// copying the shared folders before the machine starts
			bool				fStarting;
			// scripted power ons, answered when the machine runs
			std::vector<BMessage*> fStartReplies;
			BMenu*				fConsoleMenu;
			BMenuBar*			fMenuBar;
			BGroupView*			fStatusBar;
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
			BMenuItem*			fFullScreenItem;
			BMenuItem*			fFullScreenScaleItems[3];
			bool				fFullScreen;
			// the window's frame before full screen
			BRect				fWindowedFrame;
			// the host key is down; it made a combination since it went
			// down (then its release leaves the mouse capture alone)
			bool				fHostKeyDown;
			bool				fHostComboUsed;
			BMenu*				fMediaMenu;
			BMenuItem*			fEjectFloppyItem;
			BFilePanel*			fFloppyPanel;
			// the CD-ROM drives' items (by _CdDrives()), after the floppy's
			int32				fCdItemsIndex;
			std::vector<int>	fCdItemDrives;
			std::vector<BMenuItem*> fInsertCdItems;
			std::vector<BMenuItem*> fEjectCdItems;
			BFilePanel*			fCdPanel;
			bool				fKeysDown[128];
			uint32				fModifiers;
			bool				fPowered;
			BJoystick*			fJoysticks[4];
			BMessageRunner*		fJoystickRunner;
			BMessenger			fLogTarget;		// this window, for _LogSink
			// the latest problem (in the status line for a while), the
			// ones shown in an alert since the machine was powered on
			BString				fProblem;
			bigtime_t			fProblemTime;
			std::set<std::string> fProblemsAlerted;
};

#endif	// BEBOX_MACHINE_WINDOW_H
