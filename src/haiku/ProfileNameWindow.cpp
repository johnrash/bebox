/*
 * BeBox emulator for Haiku - asks for the name of a new or renamed
 * machine profile.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "ProfileNameWindow.h"

#include "Settings.h"

#include <Button.h>
#include <LayoutBuilder.h>
#include <StringView.h>
#include <TextControl.h>

enum {
	kMsgNameChanged = 'nmch',
	kMsgAccept = 'acpt',
	kMsgCancelName = 'cncl'
};


ProfileNameWindow::ProfileNameWindow(const char* title,
	const char* explanation, const char* name, const char* renaming,
	const char* button, BMessenger target, const BMessage& message)
	:
	BWindow(BRect(0, 0, 380, 120), title, B_FLOATING_WINDOW_LOOK,
		B_MODAL_APP_WINDOW_FEEL, B_NOT_ZOOMABLE | B_NOT_RESIZABLE
			| B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
	fTarget(target),
	fMessage(message),
	fOriginal(renaming != nullptr ? renaming : "")
{
	fName = new BTextControl("Name:", name, nullptr);
	fName->SetModificationMessage(new BMessage(kMsgNameChanged));
	fName->SetExplicitMinSize(BSize(260, B_SIZE_UNSET));
	fProblem = new BStringView("problem", "");
	fProblem->SetExplicitMinSize(BSize(260, B_SIZE_UNSET));
	fOk = new BButton(button, new BMessage(kMsgAccept));
	BButton* cancel = new BButton("Cancel", new BMessage(kMsgCancelName));

	BLayoutBuilder::Group<>(this, B_VERTICAL)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(new BStringView("explanation", explanation))
		.Add(fName)
		.Add(fProblem)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(cancel)
			.Add(fOk)
		.End();
	fOk->MakeDefault(true);
	fName->MakeFocus(true);
	fName->TextView()->SelectAll();
	_Check();
	CenterOnScreen();
}


void
ProfileNameWindow::_Check()
{
	bool unchanged = !fOriginal.IsEmpty() && fOriginal == fName->Text();
	BString problem;
	if (!unchanged)
		problem = Settings::CheckProfileName(fName->Text());
	fProblem->SetText(problem.String());
	fOk->SetEnabled(problem.IsEmpty() && !unchanged);
}


void
ProfileNameWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgNameChanged:
			_Check();
			break;
		case kMsgAccept:
		{
			_Check();
			if (!fOk->IsEnabled())
				break;
			BMessage accepted(fMessage);
			accepted.AddString("name", fName->Text());
			fTarget.SendMessage(&accepted);
			Quit();
			break;
		}
		case kMsgCancelName:
			Quit();
			break;
		default:
			BWindow::MessageReceived(message);
			break;
	}
}
