/*
 * BeBox emulator for Haiku - creating a new hard disk image.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Asks for the size and the format (QCOW2, which only takes the space
 * that is written, or raw), and the file: a name in the given folder (when
 * opened from a file panel), else with a save panel. The target gets
 * kMsgNewDiskCreated with "path" and, if given, "which".
 */
#ifndef BEBOX_NEW_DISK_WINDOW_H
#define BEBOX_NEW_DISK_WINDOW_H

#include <Entry.h>
#include <Messenger.h>
#include <Window.h>

class BFilePanel;
class BPopUpMenu;
class BTextControl;

class NewDiskWindow : public BWindow {
public:
								NewDiskWindow(BMessenger target,
									int32 which = -1,
									const entry_ref* folder = nullptr);
	virtual						~NewDiskWindow();

	virtual	void				MessageReceived(BMessage* message);

private:
			bool				_IsQcow() const;
			void				_FormatChanged();
			void				_CreateInFolder(bool replace);
			void				_Create(const char* path);

			BMessenger			fTarget;
			int32				fWhich;
			entry_ref			fFolder;
			bool				fHasFolder;
			BTextControl*		fName;
			BTextControl*		fSize;
			BPopUpMenu*			fFormat;
			BFilePanel*			fPanel;
};

#endif	// BEBOX_NEW_DISK_WINDOW_H
