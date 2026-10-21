/*
 * BeBox emulator for Haiku - preferences.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * A sidebar of sections (with the icons of Haiku's own preferences) and
 * a page per section. OK applies the changes; changes to the hardware
 * restart the machine, a new floppy or CD goes into the running one.
 */
#ifndef BEBOX_PREFS_WINDOW_H
#define BEBOX_PREFS_WINDOW_H

#include "Settings.h"

#include <Messenger.h>
#include <Window.h>

class BCardLayout;
class BCheckBox;
class BFilePanel;
class BListView;
class BMenuField;
class BPopUpMenu;
class BSlider;
class BTextControl;
class BView;

class PrefsWindow : public BWindow {
public:
								PrefsWindow(Settings& settings,
									BMessenger target);
	virtual						~PrefsWindow();

	virtual	void				MessageReceived(BMessage* message);

private:
			BView*				_MachinePage();
			BView*				_RomPage();
			BView*				_DisplayPage();
			BView*				_StoragePage();
			BView*				_SoundPage();
			BView*				_SerialPage();
			BView*				_GamePortsPage();
			BView*				_InputPage();
			BView*				_NetworkPage();
			BView*				_AdvancedPage();
			void				_AddSection(const char* name,
									const char* iconSignature, BView* page);

			BMenuField*			_PopUp(const char* label,
									const char* const* items,
									const int32* values, int count,
									int32 current, BPopUpMenu** menu,
									uint32 what = 'nop ');
			void				_UpdateMemoryMenu(bool hobbit, int32 current);
			int32				_Selected(BPopUpMenu* menu) const;
			void				_Browse(int32 which, bool directory);
			void				_BootRomChosen(const char* path);
			void				_UpdateSerialHints();
			void				_Apply();

			Settings&			fSettings;
			BMessenger			fTarget;
			BListView*			fSections;
			BCardLayout*		fPages;

			BPopUpMenu*			fModel;
			BPopUpMenu*			fMemory;
			BPopUpMenu*			fCpus;
			BPopUpMenu*			fBoardRevision;
			BPopUpMenu*			fIoCardRevision;
			BPopUpMenu*			fSpeed;
			BTextControl*		fRomFolder;
			BTextControl*		fBootRom;
			BTextControl*		fHobbitKernel;
			BPopUpMenu*			fVideo;
			BPopUpMenu*			fVram;
			BPopUpMenu*			fLedStyle;
			BCheckBox*			fShowLeds;
			BTextControl*		fIde[2];
			BCheckBox*			fIdeReadOnly[2];
			BPopUpMenu*			fScsiKind[7];
			BTextControl*		fScsi[7];
			BCheckBox*			fScsiReadOnly[7];
			BTextControl*		fFloppy;
			BCheckBox*			fFloppyProtected;
			BCheckBox*			fSound;
			BSlider*			fVolume;
			BPopUpMenu*			fSerialKind[4];
			BPopUpMenu*			fJoystick[4];
			BPopUpMenu*			fHostKey;
			BCheckBox*			fEthernet;
			BPopUpMenu*			fEthernetPort;
			BPopUpMenu*			fEthernetIrq;
			BTextControl*		fMacAddress;
			BTextControl*		fPortForwards;
			BSlider*			fMouseSpeed;
			BCheckBox*			fSwapButtons;
			BCheckBox*			fCaptureOnClick;
			BTextControl*		fSerial[4];
			BPopUpMenu*			fQuantum;
			BPopUpMenu*			fCpi;
			BCheckBox*			fThreads;
			BFilePanel*			fPanel;
			int32				fBrowsing;
};

#endif	// BEBOX_PREFS_WINDOW_H
