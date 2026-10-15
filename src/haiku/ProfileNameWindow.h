/*
 * BeBox emulator for Haiku - asks for the name of a new or renamed
 * machine profile.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The target gets a copy of the message with "name" added once the name
 * is accepted (Settings::CheckProfileName). 'renaming' is the profile's
 * current name when it is renamed.
 */
#ifndef BEBOX_PROFILE_NAME_WINDOW_H
#define BEBOX_PROFILE_NAME_WINDOW_H

#include <Message.h>
#include <Messenger.h>
#include <Window.h>

class BButton;
class BStringView;
class BTextControl;

class ProfileNameWindow : public BWindow {
public:
								ProfileNameWindow(const char* title,
									const char* explanation,
									const char* name, const char* renaming,
									const char* button,
									BMessenger target,
									const BMessage& message);

	virtual	void				MessageReceived(BMessage* message);

private:
			void				_Check();

			BMessenger			fTarget;
			BMessage			fMessage;
			BString				fOriginal;
			BTextControl*		fName;
			BStringView*		fProblem;
			BButton*			fOk;
};

#endif	// BEBOX_PROFILE_NAME_WINDOW_H
