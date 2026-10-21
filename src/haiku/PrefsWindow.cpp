/*
 * BeBox emulator for Haiku - preferences.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "PrefsWindow.h"

#include "BeBox.h"
#include "NewDiskWindow.h"

#include "core/storage/BfsReader.h"

#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <Box.h>
#include <Button.h>
#include <CardLayout.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FilePanel.h>
#include <GridLayout.h>
#include <IconUtils.h>
#include <Joystick.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MimeType.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Resources.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <Slider.h>
#include <StringView.h>
#include <TextControl.h>

#include <vector>

#include <string.h>

using bebox::MachineConfig;

enum {
	kMsgOk = 'okay',
	kMsgCancel = 'cncl',
	kMsgBrowse = 'brws',
	kMsgChosen = 'chos',
	kMsgSection = 'sect',
	kMsgNewDisk = 'nwdk',
	kMsgNewDiskInPanel = 'nwdp',
	kMsgSerialKind = 'srkd',
	kMsgResetFlashRequest = 'rfrq',
	kMsgClearNvramRequest = 'cnrq',
	kMsgResetConfirmed = 'rscf',
	kMsgModel = 'modl'
};

enum {
	kBrowseRoms = 0,
	kBrowseIde0 = 1,			// 1-2
	kBrowseScsi0 = 3,			// 3-9
	kBrowseFloppy = 10,
	kBrowseBootRom = 11,
	kBrowseHobbitKernel = 12
};

// Where the BFS releases keep their boot ROM update.
static const char* const kBootRomPath = "/beos/etc/bebox_bootmain.image";

// serial destination kinds (prefixes of the settings string)
static const char* const kSerialKinds[] = { "", "console", "tcp-listen:",
	"tcp-connect:", "host:", "file:" };
static const char* const kSerialLabels[] = { "Not connected",
	"Console window", "TCP server", "TCP client", "Host serial port",
	"Log to file" };
static const char* const kSerialHints[] = { "", "",
	"Port, or address:port (e.g. 2001)", "Host:port (e.g. localhost:2001)",
	"Device (e.g. /dev/ports/usb0)", "File path" };


// A sidebar entry: icon and name.
class SectionItem : public BListItem {
public:
	// 'signatures' lists alternatives separated by '|' (the first one
	// installed gives the icon); "rsrc:NAME" is a vector icon among the
	// application's resources.
	SectionItem(const char* name, const char* signatures)
		:
		fName(name),
		fIcon(nullptr)
	{
		float size = be_control_look->ComposeIconSize(24).Height() + 1;
		BString list(signatures);
		int32 start = 0;
		while (fIcon == nullptr && start < list.Length()) {
			int32 end = list.FindFirst('|', start);
			if (end < 0)
				end = list.Length();
			BString signature;
			list.CopyInto(signature, start, end - start);
			start = end + 1;
			if (signature.StartsWith("rsrc:")) {
				size_t length = 0;
				const void* data = BApplication::AppResources()->LoadResource(
					'VICN', signature.String() + 5, &length);
				if (data == nullptr)
					continue;
				fIcon = new BBitmap(BRect(0, 0, size - 1, size - 1),
					B_RGBA32);
				if (BIconUtils::GetVectorIcon((const uint8*)data, length,
						fIcon) != B_OK) {
					delete fIcon;
					fIcon = nullptr;
				}
				continue;
			}
			BMimeType type(signature.String());
			uint8* data = nullptr;
			size_t length = 0;
			if (type.GetIcon(&data, &length) != B_OK)
				continue;
			fIcon = new BBitmap(BRect(0, 0, size - 1, size - 1), B_RGBA32);
			if (BIconUtils::GetVectorIcon(data, length, fIcon) != B_OK) {
				delete fIcon;
				fIcon = nullptr;
			}
			delete[] data;
		}
	}

	float PreferredWidth() const
	{
		float iconSize = be_control_look->ComposeIconSize(24).Height() + 1;
		return be_plain_font->StringWidth(fName.String()) + iconSize
			+ 3 * be_control_look->DefaultLabelSpacing();
	}

	~SectionItem()
	{
		delete fIcon;
	}

	virtual void DrawItem(BView* owner, BRect frame, bool complete)
	{
		rgb_color background = IsSelected()
			? ui_color(B_LIST_SELECTED_BACKGROUND_COLOR)
			: ui_color(B_LIST_BACKGROUND_COLOR);
		owner->SetHighColor(background);
		owner->FillRect(frame);
		float iconSize = be_control_look->ComposeIconSize(24).Height() + 1;
		float x = frame.left + be_control_look->DefaultLabelSpacing();
		if (fIcon != nullptr) {
			owner->SetDrawingMode(B_OP_ALPHA);
			owner->DrawBitmap(fIcon, BPoint(x,
				frame.top + (frame.Height() - iconSize) / 2));
			owner->SetDrawingMode(B_OP_COPY);
		}
		x += iconSize + be_control_look->DefaultLabelSpacing();
		font_height height;
		owner->GetFontHeight(&height);
		owner->SetHighColor(IsSelected()
			? ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR)
			: ui_color(B_LIST_ITEM_TEXT_COLOR));
		owner->SetLowColor(background);
		owner->DrawString(fName.String(), BPoint(x, frame.top
			+ (frame.Height() + height.ascent - height.descent) / 2));
	}

	virtual void Update(BView* owner, const BFont* font)
	{
		BListItem::Update(owner, font);
		float iconSize = be_control_look->ComposeIconSize(24).Height() + 1;
		SetHeight(std::max(Height(), iconSize) + 8);
		SetWidth(Width() + iconSize + 3 * be_control_look->DefaultLabelSpacing());
	}

private:
	BString fName;
	BBitmap* fIcon;
};


PrefsWindow::PrefsWindow(Settings& settings, BMessenger target)
	:
	BWindow(BRect(100, 100, 760, 560), "BeBox preferences",
		B_TITLED_WINDOW, B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS
			| B_CLOSE_ON_ESCAPE),
	fSettings(settings),
	fTarget(target),
	fPanel(nullptr),
	fBrowsing(-1)
{
	fSections = new BListView("sections");
	fSections->SetSelectionMessage(new BMessage(kMsgSection));
	BScrollView* sectionScroll = new BScrollView("scroll", fSections, 0,
		false, false);
	BView* pageHolder = new BView("pages", 0);
	fPages = new BCardLayout();
	pageHolder->SetLayout(fPages);

	BString title;
	title.SetToFormat("BeBox preferences: %s", fSettings.profile.String());
	SetTitle(title.String());

	_AddSection("Machine", "rsrc:machine|application/x-vnd.Haiku-About",
		_MachinePage());
	_AddSection("ROM & NVRAM", "application/x-vnd.Haiku-Devices",
		_RomPage());
	_AddSection("Display", "application/x-vnd.Haiku-Screen", _DisplayPage());
	_AddSection("Storage", "application/x-vnd.Haiku-DriveSetup",
		_StoragePage());
	_AddSection("Sound", "application/x-vnd.Haiku-Media", _SoundPage());
	_AddSection("Keyboard & mouse", "application/x-vnd.Haiku-Input"
		"|application/x-vnd.Haiku-Mouse", _InputPage());
	_AddSection("Game ports", "application/x-vnd.Haiku-Joysticks"
		"|rsrc:joystick", _GamePortsPage());
	_AddSection("Network", "application/x-vnd.Haiku-Network", _NetworkPage());
	_AddSection("Serial ports", "application/x-vnd.Haiku-SerialConnect"
		"|application/x-vnd.Haiku-Terminal", _SerialPage());
	_AddSection("Advanced", "application/x-vnd.Haiku-Debugger",
		_AdvancedPage());

	BButton* ok = new BButton("OK", new BMessage(kMsgOk));
	BButton* cancel = new BButton("Cancel", new BMessage(kMsgCancel));
	BString profileNote;
	profileNote.SetToFormat("The profile \"%s\"; Keyboard & mouse and the "
		"LEDs are the same in all profiles.", fSettings.profile.String());
	BLayoutBuilder::Group<>(this, B_VERTICAL)
		.SetInsets(B_USE_WINDOW_SPACING)
		.AddGroup(B_HORIZONTAL)
			.Add(sectionScroll, 0)
			.Add(pageHolder, 1)
		.End()
		.Add(new BSeparatorView(B_HORIZONTAL))
		.AddGroup(B_HORIZONTAL)
			.AddGroup(B_VERTICAL, 0)
				.Add(new BStringView("profile", profileNote.String()))
				.Add(new BStringView("note", "Changes to the hardware "
					"restart the machine (not a new floppy or CD)."))
			.End()
			.AddGlue()
			.Add(cancel)
			.Add(ok)
		.End();
	float width = 0;
	for (int32 i = 0; i < fSections->CountItems(); i++) {
		width = std::max(width,
			((SectionItem*)fSections->ItemAt(i))->PreferredWidth());
	}
	sectionScroll->SetExplicitMinSize(BSize(width + 16, 300));
	sectionScroll->SetExplicitMaxSize(BSize(width + 16, B_SIZE_UNLIMITED));
	fSections->Select(0);
	ok->MakeDefault(true);
	_UpdateSerialHints();
	CenterOnScreen();
}


PrefsWindow::~PrefsWindow()
{
	delete fPanel;
}


void
PrefsWindow::_AddSection(const char* name, const char* iconSignature,
	BView* page)
{
	fSections->AddItem(new SectionItem(name, iconSignature));
	fPages->AddView(page);
}


// #pragma mark - pages


BView*
PrefsWindow::_MachinePage()
{
	const MachineConfig& machine = fSettings.machine;
	static const char* const kModels[] = { "Dual603-66 (2 \xc3\x97 PowerPC "
		"603, 66 MHz)", "Dual603e-133 (2 \xc3\x97 PowerPC 603e, 133 MHz)",
		"Hobbit prototype (2 \xc3\x97 AT&T 92010, 25 MHz)" };
	static const int32 kModelValues[] = { 66, 133, 25 };
	BMenuField* model = _PopUp("Model:", kModels, kModelValues, 3,
		machine.model == MachineConfig::kBeBox133 ? 133
			: machine.model == MachineConfig::kHobbit ? 25 : 66, &fModel,
		kMsgModel);
	fMemory = new BPopUpMenu("");
	BMenuField* memory = new BMenuField("Memory:", fMemory);
	_UpdateMemoryMenu(machine.model == MachineConfig::kHobbit,
		machine.ramMB);
	static const char* const kCpus[] = { "Two (as built)",
		"One (CPU 1 held in reset)" };
	static const int32 kCpuValues[] = { 2, 1 };
	BMenuField* cpus = _PopUp("Processors:", kCpus, kCpuValues, 2,
		machine.cpuCount, &fCpus);
	static const char* const kRevisions[] = { "Revision 5 (128 KB boot ROM "
		"flash)", "Revision 6 (512 KB flash)", "Revision 3" };
	static const int32 kRevisionValues[] = { 5, 6, 3 };
	BMenuField* revision = _PopUp("Motherboard:", kRevisions,
		kRevisionValues, 3, machine.boardRevision, &fBoardRevision);
	static const char* const kIoCards[] = { "Revision 5", "Revision 4" };
	static const int32 kIoCardValues[] = { 5, 4 };
	BMenuField* ioCard = _PopUp("I/O card:", kIoCards, kIoCardValues, 2,
		machine.ioCardRevision, &fIoCardRevision);
	static const char* const kSpeeds[] = { "50%", "Real time", "200%",
		"As fast as possible" };
	static const int32 kSpeedValues[] = { 50, 100, 200, 0 };
	BMenuField* speed = _PopUp("Speed:", kSpeeds, kSpeedValues, 4,
		fSettings.speedLimit, &fSpeed);

	BView* page = new BView("machine", 0);
	BLayoutBuilder::Grid<>(page)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.AddMenuField(model, 0, 0)
		.AddMenuField(memory, 0, 1)
		.AddMenuField(cpus, 0, 2)
		.AddMenuField(revision, 0, 3)
		.AddMenuField(ioCard, 0, 4)
		.AddMenuField(speed, 0, 5)
		.Add(new BStringView("hobbit", "The Hobbit prototype takes the "
			"memory sizes its kernel knows (4 to 32 MB)."), 0, 6, 2)
		.Add(new BStringView("hobbit2", "Motherboard and I/O card are "
			"BeBox options."), 0, 7, 2)
		.AddGlue(0, 8, 2);
	return page;
}


BView*
PrefsWindow::_RomPage()
{
	fRomFolder = new BTextControl("ROM folder:",
		fSettings.romDirectory.String(), nullptr);
	fRomFolder->SetExplicitMinSize(BSize(260, B_SIZE_UNSET));
	BMessage* browse = new BMessage(kMsgBrowse);
	browse->AddInt32("which", kBrowseRoms);
	BButton* choose = new BButton("Choose" B_UTF8_ELLIPSIS, browse);
	fBootRom = new BTextControl("Boot ROM image:",
		fSettings.bootRomImage.String(), nullptr);
	browse = new BMessage(kMsgBrowse);
	browse->AddInt32("which", kBrowseBootRom);
	BButton* chooseImage = new BButton("Choose" B_UTF8_ELLIPSIS, browse);
	fHobbitKernel = new BTextControl("Hobbit kernel:",
		fSettings.machine.hobbitKernelPath.c_str(), nullptr);
	browse = new BMessage(kMsgBrowse);
	browse->AddInt32("which", kBrowseHobbitKernel);
	BButton* chooseKernel = new BButton("Choose" B_UTF8_ELLIPSIS, browse);
	BButton* resetFlash = new BButton("Reset boot ROM flash" B_UTF8_ELLIPSIS,
		new BMessage(kMsgResetFlashRequest));
	BButton* clearNvram = new BButton("Clear NVRAM" B_UTF8_ELLIPSIS,
		new BMessage(kMsgClearNvramRequest));

	BView* page = new BView("rom", 0);
	BLayoutBuilder::Group<>(page, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.AddGrid()
			.AddTextControl(fRomFolder, 0, 0)
			.Add(choose, 2, 0)
			.AddTextControl(fBootRom, 0, 1)
			.Add(chooseImage, 2, 1)
		.End()
		.Add(new BStringView("roms", "The folder holds bootnub.rom and "
			"bootmain.rom from a BeBox."))
		.Add(new BStringView("image", "A boot ROM image replaces "
			"bootmain.rom: BeOS AA and later need one. Choose"))
		.Add(new BStringView("image2", "a bebox_bootmain.image, or a BeOS "
			"PowerPC CD image (PR2, R3, R4.5) to copy it from."))
		.AddStrut(B_USE_DEFAULT_SPACING)
		.AddGroup(B_HORIZONTAL)
			.Add(resetFlash)
			.Add(clearNvram)
			.AddGlue()
		.End()
		.Add(new BStringView("flash", "The flash holds the boot ROM (and "
			"upgrades installed by BeOS); the NVRAM the boot preferences."))
		.AddStrut(B_USE_DEFAULT_SPACING)
		.AddGrid()
			.AddTextControl(fHobbitKernel, 0, 0)
			.Add(chooseKernel, 2, 0)
		.End()
		.Add(new BStringView("hobbit", "No boot ROM of the Hobbit prototype "
			"survives: the emulator starts"))
		.Add(new BStringView("hobbit2", "/system/kernel of the boot disk (the "
			"floppy, else the first SCSI disk),"))
		.Add(new BStringView("hobbit3", "or this kernel file when one is "
			"chosen."))
		.AddGlue();
	return page;
}


BView*
PrefsWindow::_DisplayPage()
{
	const MachineConfig& machine = fSettings.machine;
	static const char* const kVideo[] = {
		"Number Nine #9 GXE64 (S3 Vision864)", "Cirrus Logic CL-GD5434",
		"None" };
	static const int32 kVideoValues[] = { MachineConfig::kVideoS3Vision864,
		MachineConfig::kVideoCirrus5434, MachineConfig::kVideoNone };
	BMenuField* video = _PopUp("Video card:", kVideo, kVideoValues, 3,
		machine.videoCard, &fVideo);
	static const char* const kVram[] = { "1 MB", "2 MB", "4 MB" };
	static const int32 kVramValues[] = { 1, 2, 4 };
	BMenuField* vram = _PopUp("Video memory:", kVram, kVramValues, 3,
		machine.vramMB, &fVram);
	static const char* const kStyles[] = { "Authentic (lime and amber)",
		"Red and green" };
	static const int32 kStyleValues[] = { kLedStyleAuthentic,
		kLedStyleRedGreen };
	BMenuField* style = _PopUp("LED colors:", kStyles, kStyleValues, 2,
		fSettings.ledStyle, &fLedStyle);
	fShowLeds = new BCheckBox("Show the front panel LEDs");
	fShowLeds->SetValue(fSettings.showLeds);

	BView* page = new BView("display", 0);
	BLayoutBuilder::Grid<>(page)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.AddMenuField(video, 0, 0)
		.AddMenuField(vram, 0, 1)
		.AddMenuField(style, 0, 2)
		.Add(fShowLeds, 1, 3)
		.AddGlue(0, 4, 2);
	return page;
}


BView*
PrefsWindow::_StoragePage()
{
	const MachineConfig& machine = fSettings.machine;
	BView* page = new BView("storage", 0);
	BGridLayout* grid = BLayoutBuilder::Grid<>(page)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.SetColumnWeight(2, 1.0f);

	int row = 0;
	for (int i = 0; i < 2; i++, row++) {
		fIde[i] = new BTextControl(nullptr, machine.ideImage[i].c_str(),
			nullptr);
		fIdeReadOnly[i] = new BCheckBox("Read only");
		fIdeReadOnly[i]->SetValue(machine.ideReadOnly[i]);
		BMessage* browse = new BMessage(kMsgBrowse);
		browse->AddInt32("which", kBrowseIde0 + i);
		grid->AddView(new BStringView("label", i == 0 ? "IDE master:"
			: "IDE slave:"), 0, row);
		grid->AddView(new BStringView("kind", "Disk"), 1, row);
		grid->AddView(fIde[i], 2, row);
		grid->AddView(new BButton("Choose" B_UTF8_ELLIPSIS, browse), 3, row);
		grid->AddView(fIdeReadOnly[i], 4, row);
	}
	static const char* const kKinds[] = { "None", "Disk", "CD-ROM",
		"Shared folder" };
	static const int32 kKindValues[] = { MachineConfig::ScsiTarget::kNone,
		MachineConfig::ScsiTarget::kDisk, MachineConfig::ScsiTarget::kCdRom,
		MachineConfig::ScsiTarget::kSharedFolder };
	for (int id = 0; id < 7; id++, row++) {
		const MachineConfig::ScsiTarget& target = machine.scsi[id];
		BString label;
		label.SetToFormat("SCSI ID %d:", id);
		BMenuField* kind = _PopUp(nullptr, kKinds, kKindValues, 4,
			target.kind, &fScsiKind[id]);
		fScsi[id] = new BTextControl(nullptr, target.image.c_str(), nullptr);
		fScsi[id]->SetExplicitMinSize(BSize(240, B_SIZE_UNSET));
		fScsiReadOnly[id] = new BCheckBox("Read only");
		fScsiReadOnly[id]->SetValue(target.readOnly);
		BMessage* browse = new BMessage(kMsgBrowse);
		browse->AddInt32("which", kBrowseScsi0 + id);
		grid->AddView(new BStringView("label", label.String()), 0, row);
		grid->AddView(kind, 1, row);
		grid->AddView(fScsi[id], 2, row);
		grid->AddView(new BButton("Choose" B_UTF8_ELLIPSIS, browse), 3, row);
		grid->AddView(fScsiReadOnly[id], 4, row);
	}
	fFloppy = new BTextControl(nullptr, machine.floppyImage.c_str(),
		nullptr);
	fFloppyProtected = new BCheckBox("Write protect");
	fFloppyProtected->SetValue(machine.floppyWriteProtected);
	BMessage* browseFloppy = new BMessage(kMsgBrowse);
	browseFloppy->AddInt32("which", kBrowseFloppy);
	grid->AddView(new BStringView("label", "Floppy:"), 0, row);
	grid->AddView(new BStringView("kind", "3.5\""), 1, row);
	grid->AddView(fFloppy, 2, row);
	grid->AddView(new BButton("Choose" B_UTF8_ELLIPSIS, browseFloppy), 3, row);
	grid->AddView(fFloppyProtected, 4, row);
	row++;
	BButton* newDisk = new BButton("New disk image" B_UTF8_ELLIPSIS,
		new BMessage(kMsgNewDisk));
	grid->AddView(newDisk, 2, row + 1);
	grid->AddView(new BStringView("hint", "Disk images: raw, or QCOW2 "
		"(grows as it is used). CD-ROMs: ISO, BIN/CUE."), 0, row + 2, 5);
	grid->AddView(new BStringView("share", "Shared folder: a Haiku folder "
		"BeOS sees as a disk (Advanced Access and later);"), 0, row + 3, 5);
	grid->AddView(new BStringView("share2", "BeOS's changes come back to "
		"the folder when BeOS is shut down."), 0, row + 4, 5);
	grid->AddItem(BSpaceLayoutItem::CreateGlue(), 0, row + 5, 5);
	return page;
}


BView*
PrefsWindow::_SoundPage()
{
	fSound = new BCheckBox("Play the sound output (CS4231A codec)");
	fSound->SetValue(fSettings.soundEnabled);
	fVolume = new BSlider("volume", "Volume", nullptr, 0, 100,
		B_HORIZONTAL);
	fVolume->SetValue(fSettings.soundVolume);
	fVolume->SetHashMarks(B_HASH_MARKS_BOTTOM);
	fVolume->SetHashMarkCount(11);
	fVolume->SetLimitLabels("Mute", "Full");

	BView* page = new BView("sound", 0);
	BLayoutBuilder::Group<>(page, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.Add(fSound)
		.Add(fVolume)
		.AddGlue();
	return page;
}


BView*
PrefsWindow::_InputPage()
{
	static const char* const kKeys[] = { "Right Control", "Right Alt",
		"Right Windows key", "Menu key" };
	static const int32 kKeyValues[] = { kHostKeyRightControl,
		kHostKeyRightCommand, kHostKeyRightOption, kHostKeyMenu };
	BMenuField* hostKey = _PopUp("Mouse release key:", kKeys, kKeyValues, 4,
		fSettings.hostKey, &fHostKey);
	fMouseSpeed = new BSlider("mousespeed", "Mouse speed", nullptr, 25, 400,
		B_HORIZONTAL);
	fMouseSpeed->SetValue(fSettings.mouseSpeed);
	fMouseSpeed->SetHashMarks(B_HASH_MARKS_BOTTOM);
	fMouseSpeed->SetHashMarkCount(8);
	fMouseSpeed->SetLimitLabels("Slow", "Fast");
	fSwapButtons = new BCheckBox("Swap the left and right mouse buttons");
	fSwapButtons->SetValue(fSettings.swapMouseButtons);
	fCaptureOnClick = new BCheckBox("Clicking the screen captures the mouse");
	fCaptureOnClick->SetValue(fSettings.captureOnClick);

	BView* page = new BView("input", 0);
	BLayoutBuilder::Group<>(page, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.AddGrid()
			.AddMenuField(hostKey, 0, 0)
		.End()
		.Add(new BStringView("keys", "While the window is active, all other "
			"keys go to the BeBox."))
		.Add(new BStringView("release", "The release key also captures the "
			"mouse again."))
		.AddStrut(B_USE_DEFAULT_SPACING)
		.Add(fMouseSpeed)
		.Add(fSwapButtons)
		.Add(fCaptureOnClick)
		.AddGlue();
	return page;
}


// An NE2000 compatible ISA card on a private network (NAT) through the
// host.
BView*
PrefsWindow::_NetworkPage()
{
	fEthernet = new BCheckBox("Ethernet card (NE2000 compatible, ISA)");
	fEthernet->SetValue(fSettings.ethernet);
	static const char* const kPorts[] = { "220", "240", "280", "2A0", "2C0",
		"300", "320", "340", "3C0" };
	static const int32 kPortValues[] = { 0x220, 0x240, 0x280, 0x2a0, 0x2c0,
		0x300, 0x320, 0x340, 0x3c0 };
	BMenuField* port = _PopUp("I/O port:", kPorts, kPortValues, 9,
		fSettings.ethernetPort, &fEthernetPort);
	static const char* const kIrqs[] = { "5", "9", "10", "11", "15" };
	static const int32 kIrqValues[] = { 5, 9, 10, 11, 15 };
	BMenuField* irq = _PopUp("IRQ:", kIrqs, kIrqValues, 5,
		fSettings.ethernetIrq, &fEthernetIrq);
	fMacAddress = new BTextControl("MAC address:",
		fSettings.macAddress.String(), nullptr);
	fPortForwards = new BTextControl("Forwarded host ports:",
		fSettings.portForwards.String(), nullptr);
	fPortForwards->SetToolTip("Host ports that reach the BeBox, e.g. "
		"\"tcp:2121:21, tcp:2323:23\" (host port:BeBox port)");

	BView* page = new BView("network", 0);
	BLayoutBuilder::Group<>(page, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.Add(fEthernet)
		.AddGrid()
			.AddMenuField(port, 0, 0)
			.AddMenuField(irq, 0, 1)
			.AddTextControl(fMacAddress, 0, 2)
			.AddTextControl(fPortForwards, 0, 3)
		.End()
		.AddStrut(B_USE_DEFAULT_SPACING)
		.Add(new BStringView("net1", "The BeBox is on a private network "
			"behind the host (NAT). In BeOS's Network"))
		.Add(new BStringView("net2", "preferences, add a \"Novell NE2000 "
			"compatible adapter (ISA)\" with this port"))
		.Add(new BStringView("net3", "and IRQ, and use DHCP (R4.5) or "
			"IP 10.0.2.15, mask 255.255.255.0,"))
		.Add(new BStringView("net4", "gateway 10.0.2.2 and DNS 10.0.2.3. "
			"The host is 10.0.2.2."))
		.AddGlue();
	return page;
}


// The four sticks of the two game ports (Y cables), each fed from a host
// joystick.
BView*
PrefsWindow::_GamePortsPage()
{
	BJoystick joystick;
	std::vector<BString> devices;
	for (int32 i = 0; i < joystick.CountDevices(); i++) {
		char name[B_OS_NAME_LENGTH];
		if (joystick.GetDeviceName(i, name) == B_OK)
			devices.push_back(name);
	}
	BView* page = new BView("gameports", 0);
	BGridLayout* grid = new BGridLayout();
	BLayoutBuilder::Group<>(page, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.AddGrid(grid)
		.End()
		.Add(new BStringView("ports", "Sticks 1 and 2 are on the upper game "
			"port, 3 and 4 on the lower one."))
		.Add(new BStringView("count", devices.empty()
			? "No joysticks are connected to this computer."
			: "Joysticks connected to this computer are listed."))
		.AddGlue();
	for (int i = 0; i < 4; i++) {
		fJoystick[i] = new BPopUpMenu("joystick");
		fJoystick[i]->AddItem(new BMenuItem("None", nullptr));
		bool found = fSettings.joystick[i].IsEmpty();
		for (const BString& device : devices) {
			BMenuItem* item = new BMenuItem(device.String(), nullptr);
			fJoystick[i]->AddItem(item);
			if (device == fSettings.joystick[i]) {
				item->SetMarked(true);
				found = true;
			}
		}
		if (!found) {
			// configured, but not connected now
			BMenuItem* item = new BMenuItem(fSettings.joystick[i].String(),
				nullptr);
			fJoystick[i]->AddItem(item);
			item->SetMarked(true);
		}
		if (fSettings.joystick[i].IsEmpty())
			fJoystick[i]->ItemAt(0)->SetMarked(true);
		BString label;
		label.SetToFormat("Stick %d:", i + 1);
		BMenuField* field = new BMenuField(label.String(), fJoystick[i]);
		grid->AddItem(field->CreateLabelLayoutItem(), 0, i);
		grid->AddItem(field->CreateMenuBarLayoutItem(), 1, i);
	}
	return page;
}


BView*
PrefsWindow::_SerialPage()
{
	BView* page = new BView("serial", 0);
	BGridLayout* grid = BLayoutBuilder::Grid<>(page)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.SetColumnWeight(2, 1.0f);
	static const char* const kNames[] = { "Serial 1:", "Serial 2:",
		"Serial 3:", "Serial 4 (debug):" };
	int32 values[6] = { 0, 1, 2, 3, 4, 5 };
	for (int i = 0; i < 4; i++) {
		BString destination = fSettings.serial[i];
		int32 current = 0;
		BString parameter;
		for (int k = 5; k > 0; k--) {
			if (destination.StartsWith(kSerialKinds[k])) {
				current = k;
				parameter = destination.String() + strlen(kSerialKinds[k]);
				break;
			}
		}
		BMenuField* kind = _PopUp(nullptr, kSerialLabels, values, 6,
			current, &fSerialKind[i], kMsgSerialKind);
		fSerial[i] = new BTextControl(nullptr, parameter.String(), nullptr);
		fSerial[i]->SetExplicitMinSize(BSize(240, B_SIZE_UNSET));
		grid->AddView(new BStringView("label", kNames[i]), 0, i);
		grid->AddView(kind, 1, i);
		grid->AddView(fSerial[i], 2, i);
	}
	grid->AddView(new BStringView("hint", "Serial 4 carries the kernel's "
		"debug output (Machine \xe2\x96\xb8 Restart with debug output)."),
		0, 4, 3);
	grid->AddItem(BSpaceLayoutItem::CreateGlue(), 0, 5, 3);
	return page;
}


BView*
PrefsWindow::_AdvancedPage()
{
	const MachineConfig& machine = fSettings.machine;
	static const char* const kQuanta[] = { "1 \xc2\xb5s", "5 \xc2\xb5s",
		"10 \xc2\xb5s (default)", "50 \xc2\xb5s" };
	static const int32 kQuantumValues[] = { 1, 5, 10, 50 };
	BMenuField* quantum = _PopUp("CPU time slice:", kQuanta, kQuantumValues,
		4, machine.quantumUs, &fQuantum);
	static const char* const kCpis[] = { "0.5", "1.0 (default)", "1.5",
		"2.0" };
	static const int32 kCpiValues[] = { 50, 100, 150, 200 };
	BMenuField* cpi = _PopUp("Cycles per instruction:", kCpis, kCpiValues,
		4, machine.cpiPercent, &fCpi);
	fThreads = new BCheckBox("Run each processor on a host core of its own");
	fThreads->SetValue(machine.threadPerCpu);

	BView* page = new BView("advanced", 0);
	BLayoutBuilder::Grid<>(page)
		.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
		.AddMenuField(quantum, 0, 0)
		.AddMenuField(cpi, 0, 1)
		.Add(fThreads, 1, 2)
		.Add(new BStringView("hint", "The processors run in slices of "
			"emulated time; shorter slices are more exact."), 0, 3, 2)
		.Add(new BStringView("hint2", "On cores of their own, both run at "
			"once when both are busy (up to twice as fast)."), 0, 4, 2)
		.AddGlue(0, 5, 2);
	return page;
}


// #pragma mark -


BMenuField*
PrefsWindow::_PopUp(const char* label, const char* const* items,
	const int32* values, int count, int32 current, BPopUpMenu** menu,
	uint32 what)
{
	*menu = new BPopUpMenu("");
	for (int i = 0; i < count; i++) {
		BMessage* message = new BMessage(what);
		message->AddInt32("value", values[i]);
		BMenuItem* item = new BMenuItem(items[i], message);
		(*menu)->AddItem(item);
		if (values[i] == current)
			item->SetMarked(true);
	}
	if ((*menu)->FindMarked() == nullptr)
		(*menu)->ItemAt(0)->SetMarked(true);
	return new BMenuField(label, *menu);
}


// The memory sizes the model takes: SIMM pairs on the BeBox, the sizes its
// kernel knows on the Hobbit. A size in between becomes the next smaller
// one, as the machine would use it.
void
PrefsWindow::_UpdateMemoryMenu(bool hobbit, int32 current)
{
	static const int32 kBeBoxSizes[] = { 8, 16, 24, 32, 48, 64, 96, 128,
		192, 256, 384, 512 };
	static const int32 kHobbitSizes[] = { 4, 8, 16, 20, 32 };
	const int32* sizes = hobbit ? kHobbitSizes : kBeBoxSizes;
	int count = hobbit ? 5 : 12;
	int32 selected = sizes[0];
	for (int i = 0; i < count; i++) {
		if (sizes[i] <= current)
			selected = sizes[i];
	}
	fMemory->RemoveItems(0, fMemory->CountItems(), true);
	for (int i = 0; i < count; i++) {
		BMessage* message = new BMessage('nop ');
		message->AddInt32("value", sizes[i]);
		BString label;
		label << sizes[i] << " MB";
		BMenuItem* item = new BMenuItem(label.String(), message);
		fMemory->AddItem(item);
		if (sizes[i] == selected)
			item->SetMarked(true);
	}
}


int32
PrefsWindow::_Selected(BPopUpMenu* menu) const
{
	BMenuItem* item = menu->FindMarked();
	if (item == nullptr || item->Message() == nullptr)
		return 0;
	return item->Message()->GetInt32("value", 0);
}


// Disk slots get a "New disk image" button in the panel: the new image
// goes into the slot.
void
PrefsWindow::_Browse(int32 which, bool directory)
{
	delete fPanel;
	BMessage message(kMsgChosen);
	fPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this), nullptr,
		directory ? B_DIRECTORY_NODE : B_FILE_NODE, false, &message);
	fBrowsing = which;

	BString title;
	bool disk = false;
	if (which >= kBrowseIde0 && which < kBrowseScsi0) {
		title = which == kBrowseIde0 ? "IDE master" : "IDE slave";
		disk = true;
	} else if (which >= kBrowseScsi0 && which < kBrowseScsi0 + 7) {
		int id = which - kBrowseScsi0;
		title.SetToFormat("SCSI ID %d", id);
		int32 kind = _Selected(fScsiKind[id]);
		disk = kind == MachineConfig::ScsiTarget::kNone
			|| kind == MachineConfig::ScsiTarget::kDisk;
	} else if (which == kBrowseFloppy)
		title = "Floppy";
	else if (which == kBrowseBootRom)
		title = "Boot ROM image";
	else if (which == kBrowseHobbitKernel)
		title = "Hobbit kernel";
	else if (which == kBrowseRoms)
		title = "ROM folder";
	BWindow* window = fPanel->Window();
	if (window->Lock()) {
		window->SetTitle(BString(kAppName).Append(": ").Append(title)
			.String());
		BView* background = window->ChildAt(0);
		BView* cancel = background != nullptr
			? background->FindView("cancel button") : nullptr;
		if (disk && cancel != nullptr) {
			BMessage* create = new BMessage(kMsgNewDiskInPanel);
			create->AddInt32("which", which);
			BButton* button = new BButton(BRect(0, 0, 10, 10), "new disk",
				"New disk image" B_UTF8_ELLIPSIS, create,
				B_FOLLOW_RIGHT | B_FOLLOW_BOTTOM);
			button->ResizeToPreferred();
			BRect frame = cancel->Frame();
			button->MoveTo(frame.left - button->Bounds().Width()
				- be_control_look->DefaultItemSpacing(),
				frame.top + (frame.Height() - button->Bounds().Height()) / 2);
			button->SetTarget(this);
			background->AddChild(button);
		}
		window->Unlock();
	}
	fPanel->Show();
}


// Takes a boot ROM update image as is; from a BeOS CD (or disk) image,
// copies /beos/etc/bebox_bootmain.image into the settings directory.
void
PrefsWindow::_BootRomChosen(const char* path)
{
	BFile file(path, B_READ_ONLY);
	char header[12] = {};
	if (file.InitCheck() == B_OK && file.Read(header, sizeof(header)) > 0
		&& (memcmp(header, "Joy!peff", 8) == 0
			|| (memcmp(header, "BOOT", 4) == 0
				&& memcmp(header + 4, "Joy!peff", 8) == 0))) {
		fBootRom->SetText(path);
		return;
	}

	std::vector<bebox::u8> data;
	std::string error;
	if (!bebox::BfsReader::ExtractFromImage(path, kBootRomPath, data,
			error)) {
		BString text;
		text.SetToFormat("%s is neither a boot ROM image nor a BeOS CD "
			"image with %s (%s).", BPath(path).Leaf(), kBootRomPath,
			error.c_str());
		BAlert* alert = new BAlert(kAppName, text.String(), "OK", nullptr,
			nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT);
		alert->Go(nullptr);
		return;
	}

	// e.g. "BeOS Release 3 PowerPC.iso" -> "BeOS Release 3 PowerPC.rom"
	BString name = BPath(path).Leaf();
	int32 dot = name.FindLast('.');
	if (dot > 0)
		name.Truncate(dot);
	BString directory = Settings::Directory();
	directory << "/BootROMs";
	create_directory(directory.String(), 0755);
	BString target = directory;
	target << "/" << name << ".rom";
	BFile output(target.String(), B_WRITE_ONLY | B_CREATE_FILE
		| B_ERASE_FILE);
	if (output.InitCheck() != B_OK
		|| output.Write(data.data(), data.size()) != (ssize_t)data.size()) {
		BString text;
		text.SetToFormat("Cannot write %s.", target.String());
		BAlert* alert = new BAlert(kAppName, text.String(), "OK", nullptr,
			nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT);
		alert->Go(nullptr);
		return;
	}
	fBootRom->SetText(target.String());
}


void
PrefsWindow::_UpdateSerialHints()
{
	for (int i = 0; i < 4; i++) {
		int32 kind = _Selected(fSerialKind[i]);
		fSerial[i]->SetEnabled(kind >= 2);
		fSerial[i]->SetToolTip(kSerialHints[kind]);
	}
}


void
PrefsWindow::_Apply()
{
	MachineConfig& machine = fSettings.machine;
	MachineConfig old = machine;
	int32 model = _Selected(fModel);
	machine.model = model == 133 ? MachineConfig::kBeBox133
		: model == 25 ? MachineConfig::kHobbit : MachineConfig::kBeBox66;
	BString hobbitKernel = fHobbitKernel->Text();
	hobbitKernel.Trim();
	machine.hobbitKernelPath = hobbitKernel.String();
	machine.ramMB = _Selected(fMemory);
	machine.cpuCount = _Selected(fCpus);
	machine.boardRevision = _Selected(fBoardRevision);
	machine.ioCardRevision = _Selected(fIoCardRevision);
	machine.videoCard = (MachineConfig::VideoCard)_Selected(fVideo);
	machine.vramMB = _Selected(fVram);
	machine.quantumUs = _Selected(fQuantum);
	machine.cpiPercent = _Selected(fCpi);
	machine.threadPerCpu = fThreads->Value() != 0;
	for (int i = 0; i < 2; i++) {
		machine.ideImage[i] = fIde[i]->Text();
		machine.ideReadOnly[i] = fIdeReadOnly[i]->Value() != 0;
	}
	for (int id = 0; id < 7; id++) {
		MachineConfig::ScsiTarget& target = machine.scsi[id];
		target.kind = (MachineConfig::ScsiTarget::Kind)
			_Selected(fScsiKind[id]);
		target.image = fScsi[id]->Text();
		target.readOnly = fScsiReadOnly[id]->Value() != 0
			|| target.kind == MachineConfig::ScsiTarget::kCdRom;
	}
	machine.floppyImage = fFloppy->Text();
	machine.floppyWriteProtected = fFloppyProtected->Value() != 0;
	// a disk without an image would stop the machine from starting
	Settings::DropEmptyDevices(machine);
	BString romDirectory = fRomFolder->Text();
	BString bootRomImage = fBootRom->Text();
	bootRomImage.Trim();
	// the flash holds the previous boot ROM: replace it
	bool resetFlash = bootRomImage != fSettings.bootRomImage
		|| romDirectory != fSettings.romDirectory;

	bool restart = machine.model != old.model || machine.ramMB != old.ramMB
		|| machine.cpuCount != old.cpuCount
		|| machine.boardRevision != old.boardRevision
		|| machine.ioCardRevision != old.ioCardRevision
		|| machine.videoCard != old.videoCard || machine.vramMB != old.vramMB
		|| machine.quantumUs != old.quantumUs
		|| machine.cpiPercent != old.cpiPercent
		|| machine.threadPerCpu != old.threadPerCpu
		|| machine.hobbitKernelPath != old.hobbitKernelPath
		|| romDirectory != fSettings.romDirectory || resetFlash;
	for (int i = 0; i < 2; i++) {
		restart |= machine.ideImage[i] != old.ideImage[i]
			|| machine.ideReadOnly[i] != old.ideReadOnly[i];
	}
	// the floppy and the CDs change while the machine runs
	BMessage message(kMsgPreferencesChanged);
	if (machine.floppyImage != old.floppyImage
		|| machine.floppyWriteProtected != old.floppyWriteProtected)
		message.AddBool("floppy", true);
	for (int id = 0; id < 7; id++) {
		bool cd = machine.scsi[id].kind == MachineConfig::ScsiTarget::kCdRom
			&& old.scsi[id].kind == MachineConfig::ScsiTarget::kCdRom;
		if (cd && machine.scsi[id].image != old.scsi[id].image) {
			message.AddInt32("cd", id);
			message.AddString("cd old", old.scsi[id].image.c_str());
			continue;
		}
		restart |= machine.scsi[id].kind != old.scsi[id].kind
			|| machine.scsi[id].image != old.scsi[id].image
			|| machine.scsi[id].readOnly != old.scsi[id].readOnly;
	}
	for (int i = 0; i < 4; i++) {
		int32 kind = _Selected(fSerialKind[i]);
		BString destination = kSerialKinds[kind];
		if (kind >= 2)
			destination << fSerial[i]->Text();
		restart |= destination != fSettings.serial[i];
		fSettings.serial[i] = destination;
	}
	bool ethernet = fEthernet->Value() != 0;
	int32 ethernetPort = _Selected(fEthernetPort);
	int32 ethernetIrq = _Selected(fEthernetIrq);
	BString macAddress = fMacAddress->Text();
	BString portForwards = fPortForwards->Text();
	restart |= ethernet != fSettings.ethernet
		|| ethernetPort != fSettings.ethernetPort
		|| ethernetIrq != fSettings.ethernetIrq
		|| macAddress != fSettings.macAddress
		|| portForwards != fSettings.portForwards;
	fSettings.ethernet = ethernet;
	fSettings.ethernetPort = ethernetPort;
	fSettings.ethernetIrq = ethernetIrq;
	fSettings.macAddress = macAddress;
	fSettings.portForwards = portForwards;
	fSettings.hostKey = _Selected(fHostKey);
	fSettings.mouseSpeed = fMouseSpeed->Value();
	fSettings.swapMouseButtons = fSwapButtons->Value() != 0;
	fSettings.captureOnClick = fCaptureOnClick->Value() != 0;
	for (int i = 0; i < 4; i++) {
		BMenuItem* item = fJoystick[i]->FindMarked();
		int32 index = item != nullptr ? fJoystick[i]->IndexOf(item) : 0;
		fSettings.joystick[i] = index <= 0 ? "" : item->Label();
	}
	bool sound = fSound->Value() != 0;
	restart |= sound != fSettings.soundEnabled;
	fSettings.soundEnabled = sound;
	fSettings.soundVolume = fVolume->Value();
	fSettings.romDirectory = romDirectory;
	fSettings.bootRomImage = bootRomImage;
	fSettings.ledStyle = _Selected(fLedStyle);
	fSettings.showLeds = fShowLeds->Value() != 0;
	fSettings.speedLimit = _Selected(fSpeed);

	message.AddBool("restart", restart);
	message.AddBool("reset_flash", resetFlash);
	fTarget.SendMessage(&message);
}


void
PrefsWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgSection:
		{
			int32 index = fSections->CurrentSelection();
			if (index >= 0)
				fPages->SetVisibleItem(index);
			break;
		}
		case kMsgModel:
			_UpdateMemoryMenu(_Selected(fModel) == 25, _Selected(fMemory));
			break;
		case kMsgSerialKind:
			_UpdateSerialHints();
			break;
		case kMsgBrowse:
		{
			int32 which = message->GetInt32("which", 0);
			bool folder = which == kBrowseRoms
				|| (which >= kBrowseScsi0 && which < kBrowseScsi0 + 7
					&& _Selected(fScsiKind[which - kBrowseScsi0])
						== MachineConfig::ScsiTarget::kSharedFolder);
			_Browse(which, folder);
			break;
		}
		case kMsgChosen:
		{
			entry_ref ref;
			if (message->FindRef("refs", &ref) != B_OK)
				break;
			BPath path(&ref);
			if (fBrowsing == kBrowseRoms)
				fRomFolder->SetText(path.Path());
			else if (fBrowsing == kBrowseBootRom)
				_BootRomChosen(path.Path());
			else if (fBrowsing == kBrowseHobbitKernel)
				fHobbitKernel->SetText(path.Path());
			else if (fBrowsing >= kBrowseIde0 && fBrowsing < kBrowseScsi0)
				fIde[fBrowsing - kBrowseIde0]->SetText(path.Path());
			else if (fBrowsing >= kBrowseScsi0
				&& fBrowsing < kBrowseScsi0 + 7) {
				int id = fBrowsing - kBrowseScsi0;
				fScsi[id]->SetText(path.Path());
				BEntry entry(&ref);
				if (entry.IsDirectory())
					fScsiKind[id]->ItemAt(3)->SetMarked(true);
				else if (_Selected(fScsiKind[id]) == 0) {
					// guess the kind from the name
					BString name(path.Leaf());
					name.ToLower();
					int kind = name.EndsWith(".iso") || name.EndsWith(".cue")
						|| name.EndsWith(".bin") || name.EndsWith(".toast")
						? 2 : 1;
					fScsiKind[id]->ItemAt(kind)->SetMarked(true);
				}
			} else if (fBrowsing == kBrowseFloppy)
				fFloppy->SetText(path.Path());
			break;
		}
		case kMsgNewDisk:
			(new NewDiskWindow(BMessenger(this)))->Show();
			break;
		case kMsgNewDiskInPanel:
		{
			// in the folder the panel shows, for the slot it was opened for
			if (fPanel == nullptr)
				break;
			entry_ref folder;
			fPanel->GetPanelDirectory(&folder);
			(new NewDiskWindow(BMessenger(this),
				message->GetInt32("which", -1), &folder))->Show();
			break;
		}
		case kMsgNewDiskCreated:
		{
			const char* path;
			if (message->FindString("path", &path) != B_OK)
				break;
			int32 which = message->GetInt32("which", -1);
			if (which >= kBrowseIde0 && which < kBrowseScsi0) {
				fIde[which - kBrowseIde0]->SetText(path);
				if (fPanel != nullptr && fBrowsing == which)
					fPanel->Hide();
				break;
			}
			if (which >= kBrowseScsi0 && which < kBrowseScsi0 + 7) {
				int id = which - kBrowseScsi0;
				fScsiKind[id]->ItemAt(1)->SetMarked(true);
				fScsi[id]->SetText(path);
				if (fPanel != nullptr && fBrowsing == which)
					fPanel->Hide();
				break;
			}
			// from the Storage page: into the first free disk slot
			for (int id = 0; id < 7; id++) {
				if (_Selected(fScsiKind[id]) == 0) {
					fScsiKind[id]->ItemAt(1)->SetMarked(true);
					fScsi[id]->SetText(path);
					break;
				}
			}
			break;
		}
		case kMsgResetFlashRequest:
		case kMsgClearNvramRequest:
		{
			bool flash = message->what == kMsgResetFlashRequest;
			BAlert* alert = new BAlert(kAppName, flash
				? "Reset the boot ROM flash to the ROM files? Boot ROM "
					"upgrades installed by BeOS are lost. The machine "
					"restarts."
				: "Clear the NVRAM? The boot preferences (boot disk) are "
					"lost. The machine restarts.",
				"Cancel", flash ? "Reset flash" : "Clear NVRAM", nullptr,
				B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			BMessage* confirmed = new BMessage(kMsgResetConfirmed);
			confirmed->AddBool("flash", flash);
			alert->Go(new BInvoker(confirmed, this));
			break;
		}
		case kMsgResetConfirmed:
			if (message->GetInt32("which", 0) == 1) {
				fTarget.SendMessage(message->GetBool("flash", false)
					? kMsgResetFlash : kMsgClearNvram);
			}
			break;
		case kMsgMediaChanged:
		{
			// inserted or ejected in the machine window
			const char* path;
			if (message->FindString("floppy", &path) == B_OK)
				fFloppy->SetText(path);
			int32 id;
			for (int32 i = 0; message->FindInt32("id", i, &id) == B_OK
					&& message->FindString("path", i, &path) == B_OK; i++) {
				if (id >= 0 && id < 7 && _Selected(fScsiKind[id])
						== MachineConfig::ScsiTarget::kCdRom)
					fScsi[id]->SetText(path);
			}
			break;
		}
		case kMsgOk:
			_Apply();
			Quit();
			break;
		case kMsgCancel:
			Quit();
			break;
		default:
			BWindow::MessageReceived(message);
			break;
	}
}
