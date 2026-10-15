/*
 * BeBox emulator for Haiku - creating a new hard disk image.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "NewDiskWindow.h"

#include "BeBox.h"
#include "core/storage/DiskImage.h"
#include "core/storage/Qcow2Image.h"

#include <Alert.h>
#include <Button.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <StringView.h>
#include <TextControl.h>

#include <cstdlib>
#include <cstring>
#include <unistd.h>

enum {
	kMsgCreate = 'crea',
	kMsgSave = 'save',
	kMsgCancelNew = 'cncl',
	kMsgFormat = 'frmt',
	kMsgReplace = 'rplc'
};

static const char* const kQcowName = "BeBox disk.qcow2";
static const char* const kRawName = "BeBox disk.raw";


NewDiskWindow::NewDiskWindow(BMessenger target, int32 which,
	const entry_ref* folder)
	:
	BWindow(BRect(0, 0, 360, 160), "New disk image", B_TITLED_WINDOW,
		B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS
			| B_CLOSE_ON_ESCAPE),
	fTarget(target),
	fWhich(which),
	fHasFolder(folder != nullptr),
	fName(nullptr),
	fPanel(nullptr)
{
	if (fHasFolder)
		fFolder = *folder;
	fSize = new BTextControl("Size (MB):", "500", nullptr);
	fFormat = new BPopUpMenu("");
	fFormat->AddItem(new BMenuItem("QCOW2 (grows as it is used)",
		new BMessage(kMsgFormat)));
	fFormat->AddItem(new BMenuItem("Raw (the full size at once)",
		new BMessage(kMsgFormat)));
	fFormat->ItemAt(0)->SetMarked(true);
	BMenuField* format = new BMenuField("Format:", fFormat);
	BButton* create = new BButton(fHasFolder ? "Create"
		: "Create" B_UTF8_ELLIPSIS, new BMessage(kMsgCreate));
	BButton* cancel = new BButton("Cancel", new BMessage(kMsgCancelNew));

	BLayoutBuilder::Grid<> fields;
	int row = 0;
	BString folderText;
	if (fHasFolder) {
		fName = new BTextControl("Name:", kQcowName, nullptr);
		fName->SetExplicitMinSize(BSize(240, B_SIZE_UNSET));
		folderText.SetToFormat("In %s", BPath(&fFolder).Path());
		fields.AddTextControl(fName, 0, row++);
	}
	fields.AddTextControl(fSize, 0, row);
	fields.AddMenuField(format, 0, row + 1);

	BLayoutBuilder::Group<> layout(this, B_VERTICAL);
	layout.SetInsets(B_USE_WINDOW_SPACING)
		.Add(fields.View());
	if (fHasFolder) {
		BStringView* where = new BStringView("folder", folderText.String());
		where->SetTruncation(B_TRUNCATE_MIDDLE);
		where->SetExplicitMinSize(BSize(300, B_SIZE_UNSET));
		layout.Add(where);
	}
	layout.Add(new BStringView("hint", "BeOS DR8 needs about 100 MB; later "
			"releases more."))
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(cancel)
			.Add(create)
		.End();
	create->MakeDefault(true);
	if (fName != nullptr) {
		fName->MakeFocus(true);
		// select the name without the extension
		fName->TextView()->Select(0, strlen(kQcowName) - 6);
	}
	CenterOnScreen();
}


NewDiskWindow::~NewDiskWindow()
{
	delete fPanel;
}


bool
NewDiskWindow::_IsQcow() const
{
	return fFormat->IndexOf(fFormat->FindMarked()) == 0;
}


// Keeps the name's extension in line with the format.
void
NewDiskWindow::_FormatChanged()
{
	if (fName == nullptr)
		return;
	BString name = fName->Text();
	const char* from = _IsQcow() ? ".raw" : ".qcow2";
	const char* to = _IsQcow() ? ".qcow2" : ".raw";
	if (name.EndsWith(from)) {
		name.Truncate(name.Length() - strlen(from));
		name << to;
		fName->SetText(name.String());
	}
}


void
NewDiskWindow::_CreateInFolder(bool replace)
{
	BString name = fName->Text();
	name.Trim();
	if (name.IsEmpty() || name.FindFirst('/') >= 0) {
		(new BAlert(kAppName, "Enter a file name for the disk image.",
			"OK"))->Go(nullptr);
		return;
	}
	BPath path(&fFolder);
	path.Append(name.String());
	if (!replace && BEntry(path.Path()).Exists()) {
		BString text;
		text.SetToFormat("\"%s\" exists already. Replace it?", name.String());
		BAlert* alert = new BAlert(kAppName, text.String(), "Cancel",
			"Replace", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		alert->Go(new BInvoker(new BMessage(kMsgReplace), this));
		return;
	}
	_Create(path.Path());
}


void
NewDiskWindow::_Create(const char* path)
{
	double megabytes = atof(fSize->Text());
	if (megabytes < 1 || megabytes > 1024 * 1024) {
		(new BAlert(kAppName, "Enter a size between 1 MB and 1 TB.", "OK"))
			->Go(nullptr);
		return;
	}
	bebox::u64 size = (bebox::u64)(megabytes * 1048576.0);
	size = (size + 511) / 512 * 512;
	std::string error;
	// replacing was confirmed (by the save panel or the alert)
	unlink(path);
	bool ok = _IsQcow() ? bebox::Qcow2Image::Create(path, size, error)
		: bebox::RawImage::Create(path, size, error);
	if (!ok) {
		BString text("The disk image could not be created:\n\n");
		text << error.c_str();
		(new BAlert(kAppName, text.String(), "OK", nullptr, nullptr,
			B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go(nullptr);
		return;
	}
	BMessage created(kMsgNewDiskCreated);
	created.AddString("path", path);
	if (fWhich >= 0)
		created.AddInt32("which", fWhich);
	fTarget.SendMessage(&created);
	Quit();
}


void
NewDiskWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgFormat:
			_FormatChanged();
			break;
		case kMsgCreate:
		{
			if (fHasFolder) {
				_CreateInFolder(false);
				break;
			}
			delete fPanel;
			BMessage save(kMsgSave);
			fPanel = new BFilePanel(B_SAVE_PANEL, new BMessenger(this),
				nullptr, 0, false, &save);
			fPanel->SetSaveText(_IsQcow() ? kQcowName : kRawName);
			fPanel->Show();
			break;
		}
		case kMsgReplace:
			if (message->GetInt32("which", 0) == 1)
				_CreateInFolder(true);
			break;
		case kMsgSave:
		{
			entry_ref directory;
			const char* name;
			if (message->FindRef("directory", &directory) != B_OK
				|| message->FindString("name", &name) != B_OK)
				break;
			BPath path(&directory);
			path.Append(name);
			_Create(path.Path());
			break;
		}
		case kMsgCancelNew:
			Quit();
			break;
		default:
			BWindow::MessageReceived(message);
			break;
	}
}
