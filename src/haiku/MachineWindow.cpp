/*
 * BeBox emulator for Haiku - the machine window.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "MachineWindow.h"

#include "BeBox.h"
#include "LedColumnView.h"
#include "MediaStatusView.h"
#include "PrefsWindow.h"
#include "ProfileNameWindow.h"
#include "ScreenView.h"
#include "SerialConsole.h"
#include "core/board/Machine.h"
#include "core/host/KeyCodes.h"
#include "core/host/SerialBackends.h"
#include "core/isa/Fdc82077.h"
#include "core/isa/JoystickAdc.h"
#include "core/isa/Kbc8042.h"
#include "core/pci/ScsiDevice.h"
#include "core/storage/CdImage.h"
#include "core/storage/DiskImage.h"

#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <Directory.h>
#include <File.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <GroupView.h>
#include <Joystick.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Path.h>
#include <PropertyInfo.h>
#include <Screen.h>
#include <StringList.h>
#include <StringView.h>
#include <TranslatorRoster.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <unistd.h>

static const uint32 kMsgTick = 'tick';
static const uint32 kMsgJoystickPoll = 'joyp';
static const uint32 kMsgHeldKey = 'hold';
// The host key toggles the mouse capture (with F: full screen) and is not
// passed to the BeBox (key code, modifier bit or 0), by Settings::hostKey.
struct HostKeyCode {
	uint32 key;
	uint32 modifier;
};
static const HostKeyCode kHostKeys[4] = {
	{ 0x60, B_RIGHT_CONTROL_KEY },
	{ 0x5f, B_RIGHT_COMMAND_KEY },		// right Alt
	{ 0x67, B_RIGHT_OPTION_KEY },		// right Windows key
	{ 0x68, 0 }							// Menu
};
static const char* const kHostKeyNames[4] = { "right Ctrl", "right Alt",
	"right Windows key", "Menu key" };


MachineWindow::MachineWindow(Settings& settings)
	:
	BWindow(settings.windowFrame, kAppName, B_TITLED_WINDOW,
		B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS),
	fSettings(settings),
	fFolderStarts(0),
	fFolderSyncs(0),
	fStarting(false),
	fConsoleMenu(nullptr),
	fFullScreen(false),
	fHostKeyDown(false),
	fHostComboUsed(false),
	fFloppyPanel(nullptr),
	fCdItemsIndex(0),
	fCdPanel(nullptr),
	fModifiers(0),
	fPowered(false),
	fJoystickRunner(nullptr)
{
	memset(fKeysDown, 0, sizeof(fKeysDown));
	for (int i = 0; i < 4; i++)
		fJoysticks[i] = nullptr;
	for (int i = 0; i < 4; i++)
		fConsoles[i] = new ConsoleSerial(i);

	BMenuBar* menuBar = new BMenuBar("menu");
	fMenuBar = menuBar;
	menuBar->AddItem(_BuildMachineMenu());
	menuBar->AddItem(_BuildMediaMenu());
	menuBar->AddItem(_BuildViewMenu());
	fConsoleMenu = new BMenu("Tools");
	BMenu* consoles = new BMenu("Serial console");
	for (int i = 0; i < 4; i++) {
		BString label;
		label.SetToFormat("Serial %d%s", i + 1, i == 3 ? " (debug)" : "");
		BMessage* message = new BMessage(kMsgSerialConsole);
		message->AddInt32("port", i);
		consoles->AddItem(new BMenuItem(label.String(), message,
			i == 3 ? 'D' : 0));
	}
	fConsoleMenu->AddItem(consoles);
	fConsoleMenu->AddItem(new BMenuItem("Save screenshot",
		new BMessage(kMsgScreenshot), 'S', B_SHIFT_KEY));
	menuBar->AddItem(fConsoleMenu);

	fScreen = new ScreenView(fEmulator);
	fScreen->SetMouseOptions(fSettings.mouseSpeed,
		fSettings.swapMouseButtons, fSettings.captureOnClick);
	fScreen->SetScale(settings.scale);
	fScreen->SetFullScreenScale(settings.fullScreenScale);
	fLeftLeds = new LedColumnView("cpu0 leds", false);
	fRightLeds = new LedColumnView("cpu1 leds", true);
	fLeftLeds->SetStyle(settings.ledStyle);
	fRightLeds->SetStyle(settings.ledStyle);
	fStatus = new BStringView("status", "Powered off");
	fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fMedia = new MediaStatusView();
	fStatusBar = new BGroupView(B_HORIZONTAL, 0);

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(menuBar)
		.AddGroup(B_HORIZONTAL, 0)
			.Add(fLeftLeds)
			.Add(fScreen)
			.Add(fRightLeds)
		.End()
		.AddGroup(fStatusBar)
			.SetInsets(B_USE_SMALL_SPACING, 2, B_USE_SMALL_SPACING, 2)
			.Add(fStatus)
			.Add(fMedia)
		.End();

	if (!_ShowLeds()) {
		fLeftLeds->Hide();
		fRightLeds->Hide();
	}

	BMessage tick(kMsgTick);
	new BMessageRunner(BMessenger(this), &tick, 50000);
	_UpdateMenus();
}


MachineWindow::~MachineWindow()
{
	_CloseJoysticks();
	fEmulator.Stop();
	fNetwork.reset();
	_SyncSharedFolders(false);
	_ReplyToStart(B_CANCELED);
	fSerialBackends.clear();
	for (int i = 0; i < 4; i++) {
		if (fConsoleWindows[i].IsValid()) {
			BLooper* looper;
			BWindow* window = dynamic_cast<BWindow*>(
				fConsoleWindows[i].Target(&looper));
			if (window != nullptr && window->Lock())
				window->Quit();
		}
		delete fConsoles[i];
	}
	delete fFloppyPanel;
	delete fCdPanel;
}


BMenu*
MachineWindow::_BuildMachineMenu()
{
	BMenu* menu = new BMenu("Machine");
	fPowerItem = new BMenuItem("Power on", new BMessage(kMsgPowerOn));
	menu->AddItem(fPowerItem);
	menu->AddItem(new BMenuItem("Reset (both switches)",
		new BMessage(kMsgReset)));
	menu->AddItem(new BMenuItem("Interrupt switch",
		new BMessage(kMsgInterruptSwitch)));
	// Haiku keeps Ctrl+Alt+Delete for itself (its Team Monitor)
	fCtrlAltDeleteItem = new BMenuItem("Send Ctrl+Alt+Delete",
		new BMessage(kMsgCtrlAltDelete));
	menu->AddItem(fCtrlAltDeleteItem);
	menu->AddSeparatorItem();
	menu->AddItem(new BMenuItem("Restart with boot menu",
		new BMessage(kMsgBootWithMenu)));
	menu->AddItem(new BMenuItem("Restart with debug output (serial 4)",
		new BMessage(kMsgBootWithDebug)));
	menu->AddSeparatorItem();
	fPauseItem = new BMenuItem("Pause", new BMessage(kMsgPause));
	menu->AddItem(fPauseItem);
	menu->AddSeparatorItem();
	fProfileMenu = new BMenu("Profile");
	menu->AddItem(fProfileMenu);
	_BuildProfileMenu();
	menu->AddItem(new BMenuItem("Preferences" B_UTF8_ELLIPSIS,
		new BMessage(kMsgPreferences), ','));
	menu->AddItem(new BMenuItem("About BeBox" B_UTF8_ELLIPSIS,
		new BMessage(B_ABOUT_REQUESTED)));
	menu->AddSeparatorItem();
	menu->AddItem(new BMenuItem("Quit", new BMessage(B_QUIT_REQUESTED), 'Q'));
	return menu;
}


// The profiles (the current one marked), then the commands for the
// current one.
void
MachineWindow::_BuildProfileMenu()
{
	fProfileMenu->RemoveItems(0, fProfileMenu->CountItems(), true);
	BStringList names;
	Settings::GetProfiles(names);
	for (int32 i = 0; i < names.CountStrings(); i++) {
		BMessage* message = new BMessage(kMsgSwitchProfile);
		message->AddString("name", names.StringAt(i));
		BMenuItem* item = new BMenuItem(names.StringAt(i).String(), message);
		item->SetMarked(names.StringAt(i) == fSettings.profile);
		fProfileMenu->AddItem(item);
	}
	fProfileMenu->AddSeparatorItem();
	fProfileMenu->AddItem(new BMenuItem("New profile" B_UTF8_ELLIPSIS,
		new BMessage(kMsgNewProfile)));
	BString label;
	label.SetToFormat("Rename \"%s\"" B_UTF8_ELLIPSIS,
		fSettings.profile.String());
	fProfileMenu->AddItem(new BMenuItem(label.String(),
		new BMessage(kMsgRenameProfile)));
	label.SetToFormat("Delete \"%s\"" B_UTF8_ELLIPSIS,
		fSettings.profile.String());
	BMenuItem* remove = new BMenuItem(label.String(),
		new BMessage(kMsgDeleteProfile));
	remove->SetEnabled(names.CountStrings() > 1);
	fProfileMenu->AddItem(remove);
	fProfileMenu->SetTargetForItems(this);
}


bool
MachineWindow::SwitchProfile(const char* name, bool create)
{
	if (fSettings.profile == name)
		return true;
	if (!Settings::ProfileExists(name)) {
		if (!create || !Settings::CheckProfileName(name).IsEmpty())
			return false;
		_NewProfile(name);
		return fSettings.profile == name;
	}
	bool wasPowered = fPowered || fStarting;
	_ClosePreferences();
	PowerOff();
	status_t status = fSettings.SwitchProfile(name);
	_ApplySettings();
	if (status != B_OK) {
		BString text;
		text.SetToFormat("The profile \"%s\" could not be loaded: %s",
			name, strerror(status));
		(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
		return false;
	}
	if (wasPowered)
		PowerOn();
	return true;
}


// A copy of the current profile, which becomes the current one. A running
// machine goes on with the copy's flash and NVRAM.
void
MachineWindow::_NewProfile(const char* name)
{
	_ClosePreferences();
	fSettings.Save();
	status_t status = fSettings.CopyProfile(name);
	if (status != B_OK) {
		BString text;
		text.SetToFormat("The profile \"%s\" could not be created: %s",
			name, strerror(status));
		(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
		return;
	}
	if (fPowered) {
		std::string flash = fSettings.FlashPath().String();
		std::string nvram = fSettings.NvramPath().String();
		fEmulator.Post([flash, nvram](bebox::Machine& machine) {
			std::string error;
			machine.SavePersistentState(error);		// to the old profile
			machine.SetPersistentPaths(flash, nvram);
			machine.SavePersistentState(error);
		});
	}
	_ApplySettings();
}


void
MachineWindow::_RenameProfile(const char* name)
{
	_ClosePreferences();
	status_t status = fSettings.RenameProfile(name);
	if (status != B_OK) {
		BString text;
		text.SetToFormat("The profile could not be renamed: %s",
			strerror(status));
		(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
	}
	_ApplySettings();
}


// Deletes the current profile and switches to the first of the others.
void
MachineWindow::_DeleteProfile()
{
	BStringList names;
	Settings::GetProfiles(names);
	BString old = fSettings.profile;
	BString next;
	for (int32 i = 0; i < names.CountStrings() && next.IsEmpty(); i++) {
		if (names.StringAt(i) != old)
			next = names.StringAt(i);
	}
	if (next.IsEmpty())
		return;
	_ClosePreferences();
	fSettings.SwitchProfile(next.String());
	status_t status = Settings::DeleteProfile(old.String());
	_ApplySettings();
	if (status != B_OK) {
		BString text;
		text.SetToFormat("The profile \"%s\" could not be deleted: %s",
			old.String(), strerror(status));
		(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
	}
}


// After a profile change: the settings that take effect at once.
void
MachineWindow::_ApplySettings()
{
	fEmulator.SetSpeedLimit(fSettings.speedLimit);
	fAudio.SetVolume(fSettings.soundVolume / 100.0f);
	_BuildProfileMenu();
	_UpdateMenus();
	_UpdateStatus();
	// the LEDs go with the profile's model (none on the Hobbit)
	if (_UpdateLeds())
		_FitWindow();
}


void
MachineWindow::_ClosePreferences()
{
	if (fPrefsWindow.IsValid())
		fPrefsWindow.SendMessage(B_QUIT_REQUESTED);
	fPrefsWindow = BMessenger();
}


BMenu*
MachineWindow::_BuildMediaMenu()
{
	BMenu* menu = new BMenu("Media");
	menu->AddItem(new BMenuItem("Insert floppy" B_UTF8_ELLIPSIS,
		new BMessage(kMsgInsertFloppy)));
	fEjectFloppyItem = new BMenuItem("Eject floppy",
		new BMessage(kMsgEjectFloppy));
	menu->AddItem(fEjectFloppyItem);
	menu->AddSeparatorItem();
	// the CD-ROM drives' items follow (_UpdateCdItems)
	fCdItemsIndex = menu->CountItems();
	fMediaMenu = menu;
	return menu;
}


// An Insert and an Eject item per CD-ROM drive, with the SCSI ID when
// there are several; disabled ones when there is none.
void
MachineWindow::_UpdateCdItems()
{
	std::vector<int> drives = _CdDrives();
	if (drives != fCdItemDrives || fInsertCdItems.empty()) {
		while (fMediaMenu->CountItems() > fCdItemsIndex)
			delete fMediaMenu->RemoveItem(fCdItemsIndex);
		fInsertCdItems.clear();
		fEjectCdItems.clear();
		size_t count = std::max<size_t>(drives.size(), 1);
		for (size_t i = 0; i < count; i++) {
			int id = i < drives.size() ? drives[i] : -1;
			BString insert("Insert CD-ROM");
			BString eject("Eject CD-ROM");
			if (drives.size() > 1) {
				BString suffix;
				suffix.SetToFormat(" (SCSI ID %d)", id);
				insert << suffix;
				eject << suffix;
			}
			insert << B_UTF8_ELLIPSIS;
			BMessage* insertMessage = new BMessage(kMsgInsertCd);
			insertMessage->AddInt32("id", id);
			BMessage* ejectMessage = new BMessage(kMsgEjectCd);
			ejectMessage->AddInt32("id", id);
			BMenuItem* insertItem = new BMenuItem(insert.String(),
				insertMessage);
			BMenuItem* ejectItem = new BMenuItem(eject.String(),
				ejectMessage);
			fMediaMenu->AddItem(insertItem);
			fMediaMenu->AddItem(ejectItem);
			fInsertCdItems.push_back(insertItem);
			fEjectCdItems.push_back(ejectItem);
		}
		fCdItemDrives = drives;
	}
	for (size_t i = 0; i < fInsertCdItems.size(); i++) {
		bool drive = i < drives.size();
		fInsertCdItems[i]->SetEnabled(drive);
		fEjectCdItems[i]->SetEnabled(drive
			&& !fSettings.machine.scsi[drives[i]].image.empty());
	}
}


BMenu*
MachineWindow::_BuildViewMenu()
{
	BMenu* menu = new BMenu("View");
	// the label names the host key (_UpdateMenus)
	fFullScreenItem = new BMenuItem("Full screen",
		new BMessage(kMsgFullScreen));
	menu->AddItem(fFullScreenItem);
	BMenu* fullScreenScale = new BMenu("Full screen scaling");
	static const char* const kFullScreenScaleNames[3] = { "Fill the screen",
		"Largest whole multiple (sharp)", "Actual size (1:1)" };
	for (int32 i = 0; i < 3; i++) {
		BMessage* message = new BMessage(kMsgFullScreenScale);
		message->AddInt32("scale", i);
		fFullScreenScaleItems[i] = new BMenuItem(kFullScreenScaleNames[i],
			message);
		fullScreenScale->AddItem(fFullScreenScaleItems[i]);
	}
	fullScreenScale->SetRadioMode(true);
	menu->AddItem(fullScreenScale);
	menu->AddSeparatorItem();
	static const char* const kScaleNames[3] = { "Fit window", "Actual size",
		"Double size" };
	for (int32 i = 0; i < 3; i++) {
		BMessage* message = new BMessage(kMsgScale);
		message->AddInt32("scale", i);
		fScaleItems[i] = new BMenuItem(kScaleNames[i], message);
		menu->AddItem(fScaleItems[i]);
	}
	menu->AddSeparatorItem();
	fShowLedsItem = new BMenuItem("Show front panel LEDs",
		new BMessage(kMsgShowLeds));
	menu->AddItem(fShowLedsItem);
	BMenu* styles = new BMenu("LED colors");
	BMessage* authentic = new BMessage(kMsgLedStyle);
	authentic->AddInt32("style", kLedStyleAuthentic);
	styles->AddItem(new BMenuItem("Authentic (lime and amber)",
		authentic));
	BMessage* redGreen = new BMessage(kMsgLedStyle);
	redGreen->AddInt32("style", kLedStyleRedGreen);
	styles->AddItem(new BMenuItem("Red and green", redGreen));
	styles->SetRadioMode(true);
	styles->ItemAt(fSettings.ledStyle == kLedStyleRedGreen ? 1 : 0)
		->SetMarked(true);
	menu->AddItem(styles);
	menu->AddSeparatorItem();
	menu->AddItem(new BMenuItem("Save screenshot",
		new BMessage(kMsgScreenshot)));
	return menu;
}


bool
MachineWindow::PowerOn()
{
	if (fPowered || fStarting)
		return true;
	// what was posted while the machine was off: it starts from the
	// settings
	fEmulator.DiscardCommands();
	bebox::MachineConfig config = fSettings.MachineConfigForRun();
	for (const bebox::MachineConfig::ScsiTarget& target : config.scsi) {
		if (target.kind == bebox::MachineConfig::ScsiTarget::kSharedFolder) {
			fStarting = true;
			_PrepareSharedFolders(config);
			_UpdateMenus();
			_UpdateStatus();
			return true;
		}
	}
	return _StartMachine(config);
}


bool
MachineWindow::_StartMachine(const bebox::MachineConfig& config)
{
	std::string error;
	ScreenView* screen = fScreen;
	// serial ports
	fSerialBackends.clear();
	bebox::SerialBackend* serial[4] = { nullptr, nullptr, nullptr, nullptr };
	for (int i = 0; i < 4; i++) {
		BString destination = fSettings.serial[i];
		if (destination == "console") {
			serial[i] = fConsoles[i];
		} else if (!destination.IsEmpty()) {
			std::string serialError;
			std::unique_ptr<bebox::SerialBackend> backend
				= bebox::CreateSerialBackend(destination.String(),
					serialError);
			if (!backend) {
				BString text;
				text.SetToFormat("Serial port %d is not connected: %s", i + 1,
					serialError.c_str());
				(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
				continue;
			}
			serial[i] = backend.get();
			fSerialBackends.push_back(std::move(backend));
		}
	}
	// Ethernet: the user mode network (NAT)
	fNetwork.reset();
	if (fSettings.ethernet) {
		bebox::UserNetworkConfig networkConfig;
		std::string networkError;
		if (bebox::ParsePortForwards(fSettings.portForwards.String(),
				networkConfig.forwards, networkError))
			fNetwork = bebox::CreateUserNetwork(networkConfig, networkError);
		if (!fNetwork) {
			BString text;
			text.SetToFormat("The Ethernet card is not connected: %s",
				networkError.c_str());
			(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
		}
	}
	bebox::NetworkBackend* network = fNetwork.get();
	BMessenger window(this);
	HaikuAudio* audio = nullptr;
	if (fSettings.soundEnabled) {
		fAudio.SetVolume(fSettings.soundVolume / 100.0f);
		if (fAudio.Start() == B_OK)
			audio = &fAudio;
	}
	if (!fEmulator.Start(config, error,
			[screen, audio, serial, network, window](bebox::Machine& machine) {
				machine.SetVideoSink(screen);
				machine.SetAudioSink(audio);
				if (network != nullptr)
					machine.SetNetworkBackend(network);
				for (int i = 0; i < 4; i++) {
					if (serial[i] != nullptr)
						machine.SetSerialBackend(i, serial[i]);
				}
				// BeOS ejects a disc: the drive shows empty
				for (int id = 0; id < 7; id++) {
					bebox::ScsiCd* cd = machine.CdRom(id);
					if (cd == nullptr)
						continue;
					cd->SetEjectCallback([window, id](const std::string& path) {
						BMessage message(kMsgCdEjected);
						message.AddInt32("id", id);
						message.AddString("path", path.c_str());
						window.SendMessage(&message);
					});
				}
			})) {
		BString text("The machine could not be started:\n\n");
		text << error.c_str() << (fSettings.machine.model
				== bebox::MachineConfig::kHobbit
			? "\n\nCheck the disk images (and the Hobbit kernel) in the "
				"preferences."
			: "\n\nCheck the ROM folder and the disk images in the "
				"preferences.");
		BAlert* alert = new BAlert(kAppName, text.String(), "OK");
		alert->Go(nullptr);
		// nothing ran on the shared folders' volumes
		fSharedFolders.clear();
		return false;
	}
	fEmulator.SetSpeedLimit(fSettings.speedLimit);
	fPowered = true;
	fScreen->SetPowered(true);
	_OpenJoysticks();
	_UpdateMenus();
	return true;
}


void
MachineWindow::PowerOff()
{
	if (fStarting) {
		// the shared folders are still being copied: the machine does not
		// start (_SharedFoldersReady drops them)
		fStarting = false;
		fEmulator.DiscardCommands();
		_ReplyToStart(B_CANCELED);
		_UpdateMenus();
		_UpdateStatus();
		return;
	}
	if (!fPowered)
		return;
	fEmulator.Stop();
	fScreen->SetPowered(false);
	fAudio.Stop();
	fSerialBackends.clear();
	fNetwork.reset();
	_SyncSharedFolders(true);
	_CloseJoysticks();
	fPowered = false;
	_ReleaseAllKeys();
	fLeftLeds->SetLevels(0, false, 0);
	fRightLeds->SetLevels(0, false, 0);
	_UpdateMenus();
	_UpdateStatus();
}


void
MachineWindow::_UpdateMenus()
{
	bool on = fPowered || fStarting;
	fPowerItem->SetLabel(on ? "Power off" : "Power on");
	fPowerItem->Message()->what = on ? kMsgPowerOff : kMsgPowerOn;
	fPauseItem->SetMarked(fEmulator.IsPaused());
	fCtrlAltDeleteItem->SetEnabled(fPowered);
	for (int32 i = 0; i < 3; i++)
		fScaleItems[i]->SetMarked(fSettings.scale == (i == 0 ? 0 : i));
	fShowLedsItem->SetMarked(fSettings.showLeds);
	BString fullScreen;
	fullScreen.SetToFormat("Full screen (%s+F)",
		kHostKeyNames[_HostKeyIndex()]);
	fFullScreenItem->SetLabel(fullScreen.String());
	fFullScreenItem->SetMarked(fFullScreen);
	for (int32 i = 0; i < 3; i++)
		fFullScreenScaleItems[i]->SetMarked(fSettings.fullScreenScale == i);
	// the Hobbit prototype's kernel drives no front panel lights
	fShowLedsItem->SetEnabled(
		fSettings.machine.model != bebox::MachineConfig::kHobbit);
	fEjectFloppyItem->SetEnabled(!fSettings.machine.floppyImage.empty());
	_UpdateCdItems();
}


bool
MachineWindow::_ShowLeds() const
{
	return fSettings.showLeds
		&& fSettings.machine.model != bebox::MachineConfig::kHobbit;
}


// Shows or hides the LED columns; true if that changed.
bool
MachineWindow::_UpdateLeds()
{
	bool show = _ShowLeds();
	// the view's own state: IsHidden() also says true while the window is
	// hidden, and a Show() too many would make a later Hide() do nothing
	if (show != fLeftLeds->IsHidden(fLeftLeds))
		return false;
	if (show) {
		fLeftLeds->Show();
		fRightLeds->Show();
	} else {
		fLeftLeds->Hide();
		fRightLeds->Hide();
	}
	return true;
}


void
MachineWindow::_UpdateStatus()
{
	BString text;
	if (fStarting) {
		text.SetToFormat("%s  \xe2\x80\x94  starting: copying the shared "
			"folder" B_UTF8_ELLIPSIS, fSettings.profile.String());
	} else if (!fPowered) {
		text.SetToFormat("%s  \xe2\x80\x94  powered off",
			fSettings.profile.String());
		if (fFolderSyncs > 0) {
			text << "  \xe2\x80\x94  copying BeOS's changes to the shared "
				"folder" B_UTF8_ELLIPSIS;
		}
	} else {
		bebox::EmulatorThread::Status status = fEmulator.GetStatus();
		fLeftLeds->SetLevels(status.leds[0], true, 0);
		fRightLeds->SetLevels(status.leds[1], true, status.disk);
		text.SetToFormat("%s  \xe2\x80\x94  %s, %u MB  \xe2\x80\x94  "
			"%.0f%%, %.0f MIPS", fSettings.profile.String(),
			fSettings.machine.model == bebox::MachineConfig::kBeBox133
				? "BeBox Dual603e-133"
				: fSettings.machine.model == bebox::MachineConfig::kHobbit
				? "Hobbit BeBox" : "BeBox Dual603-66",
			(unsigned)fSettings.machine.ramMB, status.speedPercent,
			status.mips);
		if (status.paused)
			text << "  (paused)";
		const char* hostKey = kHostKeyNames[_HostKeyIndex()];
		if (fScreen->IsMouseCaptured())
			text << "  \xe2\x80\x94  " << hostKey << " releases the mouse";
		else if (fSettings.captureOnClick)
			text << "  \xe2\x80\x94  click the screen to capture the mouse";
		else
			text << "  \xe2\x80\x94  " << hostKey << " captures the mouse";
	}
	if (text != fStatus->Text())
		fStatus->SetText(text.String());

	// media icons: what is inserted, the disk LED
	const bebox::MachineConfig& machine = fSettings.machine;
	bool present[MediaStatusView::kCount] = { false, false, false };
	BString description;
	if (!machine.floppyImage.empty()) {
		present[MediaStatusView::kFloppy] = true;
		description << "Floppy: " << BPath(machine.floppyImage.c_str()).Leaf();
	} else
		description << "Floppy: empty (click to insert)";
	// a click on the CD inserts into the first empty drive
	bool clickTargetShown = false;
	for (int id = 0; id < 7; id++) {
		const bebox::MachineConfig::ScsiTarget& target = machine.scsi[id];
		if (target.kind == bebox::MachineConfig::ScsiTarget::kNone)
			continue;
		bool cd = target.kind == bebox::MachineConfig::ScsiTarget::kCdRom;
		present[cd ? MediaStatusView::kCd : MediaStatusView::kDisk]
			|= !target.image.empty();
		description << "\nSCSI " << id << (cd ? " CD-ROM: " : " disk: ")
			<< (target.image.empty() ? "empty"
				: BPath(target.image.c_str()).Leaf());
		if (cd && target.image.empty() && !clickTargetShown) {
			description << " (click to insert)";
			clickTargetShown = true;
		}
	}
	for (int i = 0; i < 2; i++) {
		if (machine.ideImage[i].empty())
			continue;
		present[MediaStatusView::kDisk] = true;
		description << "\nIDE " << (i == 0 ? "master: " : "slave: ")
			<< BPath(machine.ideImage[i].c_str()).Leaf();
	}
	bool diskActive = false;
	if (fPowered)
		diskActive = fEmulator.GetStatus().disk > 0.05f;
	fMedia->SetState(present, diskActive, description);
}


void
MachineWindow::_FitWindow()
{
	if (fSettings.scale == 0 || fFullScreen)
		return;
	InvalidateLayout();
	ResizeToPreferred();
}


// #pragma mark - keyboard


void
MachineWindow::_SendKey(uint32 haikuKey, bool down)
{
	if (haikuKey >= 128)
		return;
	uint32 code = bebox::HaikuKeyToSet2(haikuKey);
	if (code == 0)
		return;
	if (!down && !fKeysDown[haikuKey])
		return;
	fKeysDown[haikuKey] = down;
	fEmulator.Post([code, down](bebox::Machine& machine) {
		if (down)
			machine.Keyboard().KeyDown(code);
		else
			machine.Keyboard().KeyUp(code);
	});
}


void
MachineWindow::_ReleaseAllKeys()
{
	for (uint32 key = 0; key < 128; key++) {
		if (fKeysDown[key])
			_SendKey(key, false);
	}
}


void
MachineWindow::_ModifiersChanged(uint32 old, uint32 now)
{
	struct Modifier {
		uint32 mask;
		uint32 key;
	};
	static const Modifier kModifiers[] = {
		{ B_LEFT_SHIFT_KEY, 0x4b }, { B_RIGHT_SHIFT_KEY, 0x56 },
		{ B_LEFT_CONTROL_KEY, 0x5c }, { B_RIGHT_CONTROL_KEY, 0x60 },
		{ B_LEFT_COMMAND_KEY, 0x5d }, { B_RIGHT_COMMAND_KEY, 0x5f },
		{ B_LEFT_OPTION_KEY, 0x66 }, { B_RIGHT_OPTION_KEY, 0x67 },
		{ B_MENU_KEY, 0x68 }
	};
	// the host key stays with Haiku
	uint32 hostKey = kHostKeys[_HostKeyIndex()].key;
	for (const Modifier& modifier : kModifiers) {
		if (modifier.key == hostKey)
			continue;
		bool was = (old & modifier.mask) != 0;
		bool is = (now & modifier.mask) != 0;
		if (was != is)
			_SendKey(modifier.key, is);
	}
	// Lock keys: a full key press for every change of the lock state.
	struct Lock {
		uint32 mask;
		uint32 key;
	};
	static const Lock kLocks[] = {
		{ B_CAPS_LOCK, 0x3b }, { B_NUM_LOCK, 0x22 }, { B_SCROLL_LOCK, 0x0f }
	};
	for (const Lock& lock : kLocks) {
		if (((old ^ now) & lock.mask) != 0) {
			_SendKey(lock.key, true);
			_SendKey(lock.key, false);
		}
	}
	// A modifier host key toggles the mouse capture when released (unless
	// it made a combination).
	uint32 modifier = kHostKeys[_HostKeyIndex()].modifier;
	if (modifier != 0 && (old & modifier) != 0 && (now & modifier) == 0
		&& !fHostComboUsed)
		fScreen->SetMouseCaptured(!fScreen->IsMouseCaptured());
}


int32
MachineWindow::_HostKeyIndex() const
{
	return fSettings.hostKey >= 0 && fSettings.hostKey < 4
		? fSettings.hostKey : kHostKeyRightControl;
}


bool
MachineWindow::_HandleKey(BMessage* message)
{
	switch (message->what) {
		case B_KEY_DOWN:
		case B_UNMAPPED_KEY_DOWN:
		{
			int32 key;
			if (message->FindInt32("key", &key) != B_OK)
				return false;
			if (key == (int32)kHostKeys[_HostKeyIndex()].key)
				return true;
			// Auto-repeat sends the make code again, like a real keyboard.
			if (key < 128)
				fKeysDown[key] = false;
			_SendKey(key, true);
			return true;
		}
		case B_KEY_UP:
		case B_UNMAPPED_KEY_UP:
		{
			int32 key;
			if (message->FindInt32("key", &key) != B_OK)
				return false;
			if (key == (int32)kHostKeys[_HostKeyIndex()].key) {
				if (kHostKeys[_HostKeyIndex()].modifier == 0
					&& !fHostComboUsed)
					fScreen->SetMouseCaptured(!fScreen->IsMouseCaptured());
				return true;
			}
			_SendKey(key, false);
			return true;
		}
		case B_MODIFIERS_CHANGED:
		{
			int32 now = 0;
			int32 old = 0;
			message->FindInt32("modifiers", &now);
			message->FindInt32("be:old_modifiers", &old);
			_ModifiersChanged(old, now);
			fModifiers = now;
			return true;
		}
	}
	return false;
}


// Whether the host key is down, from a key or modifiers message.
void
MachineWindow::_TrackHostKey(BMessage* message)
{
	const HostKeyCode& host = kHostKeys[_HostKeyIndex()];
	bool down = fHostKeyDown;
	if (host.modifier != 0) {
		int32 modifiers;
		if (message->FindInt32("modifiers", &modifiers) == B_OK)
			down = (modifiers & host.modifier) != 0;
	} else if (message->GetInt32("key", -1) == (int32)host.key) {
		down = message->what == B_KEY_DOWN
			|| message->what == B_UNMAPPED_KEY_DOWN;
	}
	if (down && !fHostKeyDown)
		fHostComboUsed = false;
	fHostKeyDown = down;
}


// The host key with F toggles full screen, also while the machine is off;
// the F does not reach the BeBox.
bool
MachineWindow::_HandleHostCombination(BMessage* message)
{
	if (message->what != B_KEY_DOWN || !fHostKeyDown)
		return false;
	int32 character = message->GetInt32("raw_char", 0);
	if (character != 'f' && character != 'F')
		return false;
	fHostComboUsed = true;
	if (message->GetInt32("be:key_repeat", 0) == 0)
		_SetFullScreen(!fFullScreen);
	return true;
}


void
MachineWindow::DispatchMessage(BMessage* message, BHandler* handler)
{
	if (IsActive()) {
		switch (message->what) {
			case B_KEY_DOWN:
			case B_KEY_UP:
			case B_UNMAPPED_KEY_DOWN:
			case B_UNMAPPED_KEY_UP:
			case B_MODIFIERS_CHANGED:
				_TrackHostKey(message);
				if (_HandleHostCombination(message))
					return;
				if (fPowered && _HandleKey(message))
					return;
				break;
		}
	}
	BWindow::DispatchMessage(message, handler);
}


// Full screen: no border, menu bar or status line; the screen (and the
// LEDs, if shown) fill the display. The window frame comes back after.
void
MachineWindow::_SetFullScreen(bool fullScreen)
{
	if (fullScreen == fFullScreen)
		return;
	fFullScreen = fullScreen;
	if (fullScreen) {
		fWindowedFrame = Frame();
		fMenuBar->Hide();
		fStatusBar->Hide();
		fScreen->SetFullScreen(true);
		SetLook(B_NO_BORDER_WINDOW_LOOK);
		SetFlags(Flags() | B_NOT_MOVABLE | B_NOT_RESIZABLE);
		BRect frame = BScreen(this).Frame();
		MoveTo(frame.LeftTop());
		ResizeTo(frame.Width(), frame.Height());
		BString hint;
		hint.SetToFormat("%s+F leaves full screen",
			kHostKeyNames[_HostKeyIndex()]);
		hint.SetByteAt(0, toupper(hint.ByteAt(0)));
		fScreen->ShowHint(hint.String());
	} else {
		SetFlags(Flags() & ~(B_NOT_MOVABLE | B_NOT_RESIZABLE));
		SetLook(B_TITLED_WINDOW_LOOK);
		fMenuBar->Show();
		fStatusBar->Show();
		fScreen->SetFullScreen(false);
		fScreen->ShowHint("");
		MoveTo(fWindowedFrame.LeftTop());
		ResizeTo(fWindowedFrame.Width(), fWindowedFrame.Height());
		// the BeBox may have changed its resolution meanwhile
		_FitWindow();
	}
	_UpdateMenus();
}


void
MachineWindow::ScreenChanged(BRect frame, color_space mode)
{
	if (fFullScreen) {
		MoveTo(frame.LeftTop());
		ResizeTo(frame.Width(), frame.Height());
	}
	BWindow::ScreenChanged(frame, mode);
}


void
MachineWindow::WindowActivated(bool active)
{
	if (!active) {
		fScreen->SetMouseCaptured(false);
		_ReleaseAllKeys();
	}
	BWindow::WindowActivated(active);
}


void
MachineWindow::_HoldKeyDuringBoot(uint32 set2, double seconds)
{
	// The boot ROM checks the keyboard while the logo is shown; the hold
	// is timed in emulated time (the keyboard repeats the key meanwhile).
	fEmulator.Post([set2, seconds](bebox::Machine& machine) {
		machine.Keyboard().HoldKey(set2,
			(bebox::u64)(seconds * machine.TicksPerSecond()));
	});
}


BString
MachineWindow::_SaveScreenshot(const char* target)
{
	BBitmap* bitmap = fScreen->CopyFrame();
	if (bitmap == nullptr)
		return "";
	BPath path;
	if (target != nullptr)
		path.SetTo(target);
	else {
		if (find_directory(B_DESKTOP_DIRECTORY, &path) != B_OK)
			find_directory(B_USER_DIRECTORY, &path);
		BString name;
		for (int i = 1; i < 1000; i++) {
			name.SetToFormat("BeBox screenshot %d.png", i);
			BPath candidate(path.Path(), name.String());
			BEntry entry(candidate.Path());
			if (!entry.Exists()) {
				path = candidate;
				break;
			}
		}
	}
	BBitmapStream stream(bitmap);
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	BTranslatorRoster::Default()->Translate(&stream, nullptr, nullptr, &file,
		B_PNG_FORMAT);
	BBitmap* detached;
	stream.DetachBitmap(&detached);
	delete detached;
	return path.Path();
}


// #pragma mark - messages


void
MachineWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgTick:
			_UpdateStatus();
			break;
		case kMsgJoystickPoll:
			_PollJoysticks();
			break;
		case kMsgFrame:
			if (message->GetBool("resized", false))
				_FitWindow();
			break;
		case kMsgPowerOn:
			PowerOn();
			// a scripted power on is answered when the machine runs
			if (fStarting && message->IsSourceWaiting()) {
				fStartReplies.push_back(DetachCurrentMessage());
				return;
			}
			break;
		case kMsgSharedFoldersReady:
			_SharedFoldersReady(message);
			break;
		case kMsgSharedFoldersSynced:
		{
			fFolderSyncs--;
			const char* report;
			if (message->FindString("report", &report) == B_OK)
				(new BAlert(kAppName, report, "OK"))->Go(nullptr);
			_UpdateStatus();
			break;
		}
		case kMsgPowerOff:
			PowerOff();
			break;
		case kMsgReset:
			if (!fPowered) {
				PowerOn();
				break;
			}
			_ReleaseAllKeys();
			fEmulator.Post([](bebox::Machine& machine) {
				machine.PowerOn();
			});
			break;
		case kMsgTypeKeys:
		{
			const char* text;
			if (message->FindString("text", &text) == B_OK && fPowered)
				_TypeText(text);
			break;
		}
		case B_GET_PROPERTY:
		case B_SET_PROPERTY:
			if (!_HandleScripting(message))
				BWindow::MessageReceived(message);
			return;
		case kMsgInterruptSwitch:
			fEmulator.Post([](bebox::Machine& machine) {
				machine.PressInterruptSwitch();
			});
			break;
		case kMsgCtrlAltDelete:
			// BeOS R3 and later show their Team Monitor (twice: restart)
			if (fPowered)
				_PressKeyCombination({ "LeftControl", "LeftAlt", "Delete" });
			break;
		case kMsgBootWithMenu:
		case kMsgBootWithDebug:
		{
			if (!fPowered && !PowerOn())
				break;
			_ReleaseAllKeys();
			// F1 sends the kernel's debug output to serial port 4; it
			// goes up when the boot ROM starts writing there.
			bool debug = message->what == kMsgBootWithDebug;
			fEmulator.Post([debug](bebox::Machine& machine) {
				machine.PowerOn();
				if (debug)
					machine.HoldDebugOutputKey();
			});
			if (debug) {
				if (fSettings.serial[3] == "console")
					_ShowConsole(3);
				break;
			}
			// The boot menu: DR7/DR8 ROMs: any key; PR and later: left
			// Shift, down when the ROM looks at the keyboard (about 6 s
			// after power on with 32 MB; the memory test before takes
			// 1/16 s per MB more at 66 MHz, half that at 133 MHz).
			const bebox::MachineConfig& config = fSettings.machine;
			double memoryTest = config.ramMB > 32
				? (config.ramMB - 32) / (config.model
					== bebox::MachineConfig::kBeBox133 ? 32.0 : 16.0) : 0;
			_HoldKeyDuringBoot(0x12, 8 + memoryTest);
			break;
		}
		case kMsgHeldKey:
		{
			uint32 code = message->GetInt32("code", 0);
			int32 remaining = message->GetInt32("remaining", 0) - 1;
			if (remaining <= 0) {
				fEmulator.Post([code](bebox::Machine& machine) {
					machine.Keyboard().KeyUp(code);
				});
				break;
			}
			fEmulator.Post([code](bebox::Machine& machine) {
				machine.Keyboard().KeyDown(code);
			});
			BMessage next(kMsgHeldKey);
			next.AddInt32("code", code);
			next.AddInt32("remaining", remaining);
			BMessageRunner::StartSending(BMessenger(this), &next, 100000, 1);
			break;
		}
		case kMsgPause:
			fEmulator.SetPaused(!fEmulator.IsPaused());
			_UpdateMenus();
			break;
		case kMsgScale:
			fSettings.scale = message->GetInt32("scale", 1);
			fScreen->SetScale(fSettings.scale);
			_UpdateMenus();
			_FitWindow();
			break;
		case kMsgFullScreen:
			_SetFullScreen(!fFullScreen);
			break;
		case kMsgFullScreenScale:
		{
			int32 scale = message->GetInt32("scale", kFullScreenFill);
			if (scale < kFullScreenFill || scale > kFullScreenActual)
				break;
			fSettings.fullScreenScale = scale;
			fScreen->SetFullScreenScale(scale);
			_UpdateMenus();
			break;
		}
		case kMsgShowLeds:
			fSettings.showLeds = !fSettings.showLeds;
			_UpdateLeds();
			_UpdateMenus();
			_FitWindow();
			break;
		case kMsgLedStyle:
			fSettings.ledStyle = message->GetInt32("style", 0);
			fLeftLeds->SetStyle(fSettings.ledStyle);
			fRightLeds->SetStyle(fSettings.ledStyle);
			break;
		case kMsgScreenshot:
		{
			const char* path = message->GetString("path", nullptr);
			BString saved = _SaveScreenshot(path);
			if (message->IsSourceWaiting()) {
				BMessage reply(B_REPLY);
				reply.AddString("path", saved);
				message->SendReply(&reply);
			}
			return;
		}
		case kMsgInsertFloppy:
			if (fFloppyPanel == nullptr) {
				fFloppyPanel = new BFilePanel(B_OPEN_PANEL,
					new BMessenger(this), nullptr, B_FILE_NODE, false,
					new BMessage(kMsgFloppyChosen));
			}
			fFloppyPanel->Show();
			break;
		case kMsgFloppyChosen:
		{
			entry_ref ref;
			if (message->FindRef("refs", &ref) != B_OK)
				break;
			BPath path(&ref);
			_InsertFloppy(path.Path());
			break;
		}
		case kMsgEjectFloppy:
			_EjectFloppy();
			break;
		case kMsgInsertCd:
		{
			// from the status line: the first empty drive, else the first
			int32 id = message->GetInt32("id", -1);
			std::vector<int> drives = _CdDrives();
			for (size_t i = 0; id < 0 && i < drives.size(); i++) {
				if (fSettings.machine.scsi[drives[i]].image.empty())
					id = drives[i];
			}
			if (id < 0 && !drives.empty())
				id = drives[0];
			if (id >= 0)
				_ShowCdPanel(id);
			break;
		}
		case kMsgCdChosen:
		{
			entry_ref ref;
			int32 id = message->GetInt32("id", -1);
			if (message->FindRef("refs", &ref) != B_OK || id < 0 || id > 6)
				break;
			BPath path(&ref);
			_InsertCd(id, path.Path());
			break;
		}
		case kMsgEjectCd:
		{
			int32 id = message->GetInt32("id", -1);
			if (id >= 0 && id < 7)
				_EjectCd(id);
			break;
		}
		case kMsgCdEjected:
		{
			// by BeOS: the drive is empty unless another disc went in since
			int32 id = message->GetInt32("id", -1);
			const char* path = message->GetString("path", "");
			if (id < 0 || id > 6)
				break;
			bebox::MachineConfig::ScsiTarget& target
				= fSettings.machine.scsi[id];
			if (target.kind == bebox::MachineConfig::ScsiTarget::kCdRom
				&& target.image == path) {
				target.image.clear();
				_MediaChanged();
				_UpdateMenus();
			}
			break;
		}
		case kMsgPreferences:
		{
			BLooper* looper;
			BWindow* window = fPrefsWindow.IsValid()
				? dynamic_cast<BWindow*>(fPrefsWindow.Target(&looper))
				: nullptr;
			if (window != nullptr && window->Lock()) {
				window->Activate();
				window->Unlock();
				break;
			}
			window = new PrefsWindow(fSettings, BMessenger(this));
			fPrefsWindow = BMessenger(window);
			window->Show();
			break;
		}
		case kMsgSwitchProfile:
		{
			const char* name;
			if (message->FindString("name", &name) != B_OK
				|| fSettings.profile == name) {
				_BuildProfileMenu();
				break;
			}
			if (!fPowered || message->GetInt32("which", -1) == 1) {
				SwitchProfile(name, false);
				break;
			}
			if (message->HasInt32("which")) {
				// cancelled
				_BuildProfileMenu();
				break;
			}
			BString text;
			text.SetToFormat("Restart the BeBox with the profile \"%s\"?\n\n"
				"BeOS is not shut down first: if it has unsaved work, shut "
				"it down (Be menu > Shut Down) before switching.", name);
			BAlert* alert = new BAlert(kAppName, text.String(), "Cancel",
				"Restart", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			BMessage* confirmed = new BMessage(kMsgSwitchProfile);
			confirmed->AddString("name", name);
			alert->Go(new BInvoker(confirmed, this));
			_BuildProfileMenu();
			break;
		}
		case kMsgNewProfile:
		{
			const char* name;
			if (message->FindString("name", &name) == B_OK) {
				_NewProfile(name);
				break;
			}
			BString explanation;
			explanation.SetToFormat("The new profile starts as a copy of "
				"\"%s\".", fSettings.profile.String());
			BString suggestion(fSettings.profile);
			suggestion << " copy";
			for (int i = 2; Settings::ProfileExists(suggestion.String()); i++)
				suggestion.SetToFormat("%s copy %d", fSettings.profile.String(),
					i);
			(new ProfileNameWindow("New profile", explanation.String(),
				suggestion.String(), nullptr, "Create",
				BMessenger(this), BMessage(kMsgNewProfile)))->Show();
			break;
		}
		case kMsgRenameProfile:
		{
			const char* name;
			if (message->FindString("name", &name) == B_OK) {
				_RenameProfile(name);
				break;
			}
			if (fPowered) {
				(new BAlert(kAppName, "Power the BeBox off to rename its "
					"profile.", "OK"))->Go(nullptr);
				break;
			}
			BString explanation;
			explanation.SetToFormat("Rename the profile \"%s\":",
				fSettings.profile.String());
			(new ProfileNameWindow("Rename profile", explanation.String(),
				fSettings.profile.String(), fSettings.profile.String(),
				"Rename", BMessenger(this), BMessage(kMsgRenameProfile)))
				->Show();
			break;
		}
		case kMsgDeleteProfile:
		{
			if (message->GetInt32("which", -1) == 1) {
				_DeleteProfile();
				break;
			}
			if (message->HasInt32("which"))
				break;
			if (fPowered) {
				(new BAlert(kAppName, "Power the BeBox off to delete its "
					"profile.", "OK"))->Go(nullptr);
				break;
			}
			BString text;
			text.SetToFormat("Delete the profile \"%s\"?\n\nIts settings, "
				"boot ROM flash and NVRAM are deleted; disk images are "
				"kept.", fSettings.profile.String());
			BAlert* alert = new BAlert(kAppName, text.String(), "Cancel",
				"Delete", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			alert->Go(new BInvoker(new BMessage(kMsgDeleteProfile), this));
			break;
		}
		case kMsgSerialConsole:
			_ShowConsole(message->GetInt32("port", 3));
			break;
		case kMsgResetFlash:
		case kMsgClearNvram:
		{
			bool wasPowered = fPowered || fStarting;
			PowerOff();
			BString path = message->what == kMsgResetFlash
				? fSettings.FlashPath() : fSettings.NvramPath();
			unlink(path.String());
			if (wasPowered)
				PowerOn();
			break;
		}
		case kMsgPreferencesChanged:
		{
			fSettings.Save();
			fLeftLeds->SetStyle(fSettings.ledStyle);
			fRightLeds->SetStyle(fSettings.ledStyle);
			fEmulator.SetSpeedLimit(fSettings.speedLimit);
			fAudio.SetVolume(fSettings.soundVolume / 100.0f);
			fScreen->SetMouseOptions(fSettings.mouseSpeed,
				fSettings.swapMouseButtons, fSettings.captureOnClick);
			if (_UpdateLeds())
				_FitWindow();
			bool restart = message->GetBool("restart", false)
				&& (fPowered || fStarting);
			if (restart)
				PowerOff();
			if (message->GetBool("reset_flash", false))
				unlink(fSettings.FlashPath().String());
			if (restart)
				PowerOn();
			else if (fPowered) {
				_OpenJoysticks();
				// a new floppy or CD goes into the running machine
				bebox::MachineConfig& machine = fSettings.machine;
				if (message->GetBool("floppy", false)) {
					std::string path = machine.floppyImage;
					if (path.empty())
						_EjectFloppy();
					else
						_InsertFloppy(path.c_str());
				}
				int32 id;
				for (int32 i = 0; message->FindInt32("cd", i, &id) == B_OK;
						i++) {
					std::string path = machine.scsi[id].image;
					if (path.empty())
						_EjectCd(id);
					else if (!_InsertCd(id, path.c_str())) {
						// the old disc stays
						machine.scsi[id].image
							= message->GetString("cd old", i, "");
					}
				}
			}
			_UpdateMenus();
			break;
		}
		case B_ABOUT_REQUESTED:
			be_app->PostMessage(B_ABOUT_REQUESTED);
			break;
		case kMsgCaptureChanged:
			_UpdateStatus();
			break;
		default:
			BWindow::MessageReceived(message);
			return;
	}
	// scripted commands (hey) get their reply after the action
	if (message->IsSourceWaiting()) {
		BMessage reply(B_REPLY);
		reply.AddInt32("error", B_OK);
		message->SendReply(&reply);
	}
}


// #pragma mark - scripting


static property_info kProperties[] = {
	{ "Running", { B_GET_PROPERTY, B_SET_PROPERTY, 0 },
		{ B_DIRECT_SPECIFIER, 0 },
		"Whether the machine is on; set it to power on or off.", 0,
		{ B_BOOL_TYPE } },
	{ "Speed", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"Emulated speed in percent of real time.", 0, { B_DOUBLE_TYPE } },
	{ "Mips", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"Million instructions per second (both processors).", 0,
		{ B_DOUBLE_TYPE } },
	{ "EmulatedTime", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"Seconds of emulated time since power on.", 0, { B_DOUBLE_TYPE } },
	{ "LedLevels", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"Front panel LED levels (0-15): left, right.", 0, { B_INT32_TYPE } },
	{ "DiskLed", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"Whether the disk LED is lit.", 0, { B_BOOL_TYPE } },
	{ "Pointer", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"The emulated hardware cursor (screen pixels; -1 when hidden).", 0,
		{ B_POINT_TYPE } },
	{ "SerialLog", { B_GET_PROPERTY, 0 }, { B_INDEX_SPECIFIER, 0 },
		"Output of serial port 1-4 (ports connected to a console).", 0,
		{ B_STRING_TYPE } },
	{ "Floppy", { B_GET_PROPERTY, B_SET_PROPERTY, 0 },
		{ B_DIRECT_SPECIFIER, 0 },
		"The floppy disk image; set \"\" to eject.", 0, { B_STRING_TYPE } },
	{ "CdRom", { B_GET_PROPERTY, B_SET_PROPERTY, 0 },
		{ B_DIRECT_SPECIFIER, B_INDEX_SPECIFIER, 0 },
		"The disc image in the CD-ROM drive on a SCSI ID (or in the first "
		"drive); set \"\" to eject.", 0, { B_STRING_TYPE } },
	{ "AudioStats", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"Sound output counters (frames received, dropped, consumed, "
		"output, underruns, buffered; output rate, buffer size and rate "
		"correction).", 0,
		{ B_STRING_TYPE } },
	{ "Profile", { B_GET_PROPERTY, B_SET_PROPERTY, 0 },
		{ B_DIRECT_SPECIFIER, 0 },
		"The machine profile; set to switch (a running machine restarts).",
		0, { B_STRING_TYPE } },
	{ "Profiles", { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
		"The names of the machine profiles.", 0, { B_STRING_TYPE } },
	{ "FullScreen", { B_GET_PROPERTY, B_SET_PROPERTY, 0 },
		{ B_DIRECT_SPECIFIER, 0 },
		"Whether the window fills the screen.", 0, { B_BOOL_TYPE } },
	{ 0 }
};


BHandler*
MachineWindow::ResolveSpecifier(BMessage* message, int32 index,
	BMessage* specifier, int32 what, const char* property)
{
	BPropertyInfo info(kProperties);
	if (info.FindMatch(message, index, specifier, what, property) >= 0)
		return this;
	return BWindow::ResolveSpecifier(message, index, specifier, what,
		property);
}


status_t
MachineWindow::GetSupportedSuites(BMessage* data)
{
	data->AddString("suites", "suite/vnd.BeBox-machine");
	BPropertyInfo info(kProperties);
	data->AddFlat("messages", &info);
	return BWindow::GetSupportedSuites(data);
}


bool
MachineWindow::_HandleScripting(BMessage* message)
{
	int32 index;
	BMessage specifier;
	int32 form;
	const char* property;
	if (message->GetCurrentSpecifier(&index, &specifier, &form, &property)
			!= B_OK)
		return false;
	bool set = message->what == B_SET_PROPERTY;
	bebox::EmulatorThread::Status status = fEmulator.GetStatus();
	BMessage reply(B_REPLY);
	status_t result = B_OK;
	if (strcmp(property, "Running") == 0) {
		if (set) {
			bool on;
			if (message->FindBool("data", &on) != B_OK)
				result = B_BAD_VALUE;
			else if (on && !fPowered) {
				if (!PowerOn())
					result = B_ERROR;
				else if (fStarting) {
					// answered when the machine runs
					fStartReplies.push_back(DetachCurrentMessage());
					return true;
				}
			} else if (!on && (fPowered || fStarting))
				PowerOff();
		} else
			reply.AddBool("result", fPowered);
	} else if (strcmp(property, "Speed") == 0 && !set)
		reply.AddDouble("result", status.speedPercent);
	else if (strcmp(property, "Mips") == 0 && !set)
		reply.AddDouble("result", status.mips);
	else if (strcmp(property, "EmulatedTime") == 0 && !set)
		reply.AddDouble("result", status.emulatedSeconds);
	else if (strcmp(property, "LedLevels") == 0 && !set) {
		reply.AddInt32("result", (int32)(status.leds[0] + 0.5f));
		reply.AddInt32("result", (int32)(status.leds[1] + 0.5f));
	} else if (strcmp(property, "DiskLed") == 0 && !set)
		reply.AddBool("result", status.disk > 0.05f);
	else if (strcmp(property, "Pointer") == 0 && !set)
		reply.AddPoint("result", BPoint(status.cursorX, status.cursorY));
	else if (strcmp(property, "SerialLog") == 0 && !set) {
		int32 port = specifier.GetInt32("index", 0);
		if (port < 1 || port > 4 || fConsoles[port - 1] == nullptr)
			result = B_BAD_INDEX;
		else {
			std::string text;
			fConsoles[port - 1]->OutputSince(0, text);
			reply.AddString("result", text.c_str());
		}
	} else if (strcmp(property, "Profile") == 0) {
		if (set) {
			const char* name;
			if (message->FindString("data", &name) != B_OK)
				result = B_BAD_VALUE;
			else if (!Settings::ProfileExists(name))
				result = B_ENTRY_NOT_FOUND;
			else if (!SwitchProfile(name, false))
				result = B_ERROR;
		} else
			reply.AddString("result", fSettings.profile);
	} else if (strcmp(property, "Profiles") == 0 && !set) {
		BStringList names;
		Settings::GetProfiles(names);
		for (int32 i = 0; i < names.CountStrings(); i++)
			reply.AddString("result", names.StringAt(i));
	} else if (strcmp(property, "AudioStats") == 0 && !set) {
		HaikuAudio::Stats stats = fAudio.GetStats();
		BString text;
		text.SetToFormat("received %" B_PRIu64 " dropped %" B_PRIu64
			" consumed %" B_PRIu64 " output %" B_PRIu64 " underruns %"
			B_PRIu32 " buffered %" B_PRIu32 " rate %.0f buffer %" B_PRIu32
			" correction %+.3f%%", stats.received, stats.dropped,
			stats.consumed, stats.output, stats.underruns, stats.buffered,
			stats.outputRate, stats.bufferFrames, stats.correction * 100);
		reply.AddString("result", text);
	} else if (strcmp(property, "Floppy") == 0) {
		if (set) {
			const char* path;
			if (message->FindString("data", &path) != B_OK)
				result = B_BAD_VALUE;
			else if (path[0] == '\0')
				_EjectFloppy();
			else
				_InsertFloppy(path);
		} else
			reply.AddString("result", fSettings.machine.floppyImage.c_str());
	} else if (strcmp(property, "FullScreen") == 0) {
		if (set) {
			bool on;
			if (message->FindBool("data", &on) != B_OK)
				result = B_BAD_VALUE;
			else
				_SetFullScreen(on);
		} else
			reply.AddBool("result", fFullScreen);
	} else if (strcmp(property, "CdRom") == 0) {
		std::vector<int> drives = _CdDrives();
		int32 id = form == B_INDEX_SPECIFIER ? specifier.GetInt32("index", -1)
			: drives.empty() ? -1 : drives[0];
		if (std::find(drives.begin(), drives.end(), id) == drives.end())
			result = B_BAD_INDEX;
		else if (set) {
			const char* path;
			BString error;
			if (message->FindString("data", &path) != B_OK)
				result = B_BAD_VALUE;
			else if (path[0] == '\0')
				_EjectCd(id);
			else if (!_InsertCd(id, path, &error)) {
				result = B_ERROR;
				reply.AddString("message", error);
			}
		} else {
			reply.AddString("result",
				fSettings.machine.scsi[id].image.c_str());
		}
	} else
		return false;
	if (result != B_OK) {
		reply.what = B_MESSAGE_NOT_UNDERSTOOD;
		if (!reply.HasString("message"))
			reply.AddString("message", strerror(result));
	}
	reply.AddInt32("error", result);
	message->SendReply(&reply);
	return true;
}


void
MachineWindow::_InsertFloppy(const char* path)
{
	fSettings.machine.floppyImage = path;
	std::string file = path;
	bool writeProtected = fSettings.machine.floppyWriteProtected;
	fEmulator.Post([file, writeProtected](bebox::Machine& machine) {
		std::string error;
		std::unique_ptr<bebox::DiskImage> image
			= bebox::DiskImage::Open(file, writeProtected, error);
		if (image && machine.Floppy() != nullptr)
			machine.Floppy()->InsertDisk(machine.FloppyUnit(),
				std::move(image), writeProtected, error);
	});
	_MediaChanged();
	_UpdateMenus();
}


// Host joysticks feed the game ports while the machine runs, polled at
// 50 Hz.
void
MachineWindow::_OpenJoysticks()
{
	_CloseJoysticks();
	bool any = false;
	for (int i = 0; i < 4; i++) {
		if (fSettings.joystick[i].IsEmpty())
			continue;
		BJoystick* joystick = new BJoystick;
		if (joystick->Open(fSettings.joystick[i].String(), true) < 0
			&& joystick->Open(fSettings.joystick[i].String(), false) < 0) {
			delete joystick;
			continue;
		}
		fJoysticks[i] = joystick;
		any = true;
	}
	if (any) {
		BMessage poll(kMsgJoystickPoll);
		fJoystickRunner = new BMessageRunner(BMessenger(this), &poll, 20000);
	}
}


void
MachineWindow::_CloseJoysticks()
{
	delete fJoystickRunner;
	fJoystickRunner = nullptr;
	for (int i = 0; i < 4; i++) {
		delete fJoysticks[i];
		fJoysticks[i] = nullptr;
	}
}


void
MachineWindow::_PollJoysticks()
{
	if (!fPowered)
		return;
	for (int i = 0; i < 4; i++) {
		BJoystick* joystick = fJoysticks[i];
		if (joystick == nullptr || joystick->Update() != B_OK)
			continue;
		int16 axes[16] = {};
		if (joystick->CountAxes() >= 2)
			joystick->GetAxisValues(axes, 0);
		uint32 buttons = joystick->ButtonValues(0);
		// -32768..32767 to the 12-bit converter range
		bebox::u16 x = (bebox::u16)((axes[0] + 32768) >> 4);
		bebox::u16 y = (bebox::u16)((axes[1] + 32768) >> 4);
		bool button1 = (buttons & 1) != 0;
		bool button2 = (buttons & 2) != 0;
		fEmulator.Post([i, x, y, button1, button2](bebox::Machine& machine) {
			if (machine.GamePorts() != nullptr)
				machine.GamePorts()->SetStick(i, x, y, button1, button2);
		});
	}
}


// Shared folders: a BFS volume per folder, built in the settings folder
// at power on, BeOS's changes back to the folder at power off. The copies
// take seconds with a big folder: they run on fFolderJobs, one after the
// other (a folder goes back before it is copied again), and the machine
// starts when its folders are ready (_SharedFoldersReady).
struct MachineWindow::FolderStart {
	int32 generation = 0;
	bebox::MachineConfig config;	// the volumes' images in place
	std::string profileDirectory;
	std::vector<std::unique_ptr<bebox::SharedFolder> > folders;
	BString messages;
};


void
MachineWindow::_PrepareSharedFolders(const bebox::MachineConfig& config)
{
	std::shared_ptr<FolderStart> start(new FolderStart);
	start->generation = ++fFolderStarts;
	start->config = config;
	start->profileDirectory = fSettings.ProfileDirectory().String();
	fFolderStart = start;
	HaikuAttributes* attributes = &fHostAttributes;
	BMessenger window(this);
	fFolderJobs.Add([start, attributes, window]() {
		for (int id = 0; id < 7; id++) {
			bebox::MachineConfig::ScsiTarget& target = start->config.scsi[id];
			if (target.kind
					!= bebox::MachineConfig::ScsiTarget::kSharedFolder)
				continue;
			BString image = start->profileDirectory.c_str();
			image << "/shared-scsi" << id << ".qcow2";
			std::unique_ptr<bebox::SharedFolder> share(
				new bebox::SharedFolder(target.image, image.String(),
					attributes));
			std::string report;
			std::string error;
			if (!share->Prepare(report, error)) {
				start->messages << error.c_str() << "\n\n";
				target.kind = bebox::MachineConfig::ScsiTarget::kNone;
				continue;
			}
			if (!report.empty())
				start->messages << report.c_str() << "\n\n";
			target.image = image.String();
			target.readOnly = false;
			start->folders.push_back(std::move(share));
		}
		BMessage ready(kMsgSharedFoldersReady);
		ready.AddInt32("generation", start->generation);
		window.SendMessage(&ready);
	});
}


void
MachineWindow::_SharedFoldersReady(BMessage* message)
{
	std::shared_ptr<FolderStart> start = fFolderStart;
	if (!start || message->GetInt32("generation", -1) != start->generation)
		return;
	fFolderStart.reset();
	// powered off meanwhile: nothing ran on the volumes, they are dropped
	if (!fStarting)
		return;
	fStarting = false;
	fSharedFolders = std::move(start->folders);
	if (start->messages.Length() > 0) {
		start->messages.Trim();
		(new BAlert(kAppName, start->messages.String(), "OK"))->Go(nullptr);
	}
	// the floppy and CDs changed meanwhile go in
	bebox::MachineConfig& config = start->config;
	config.floppyImage = fSettings.machine.floppyImage;
	config.floppyWriteProtected = fSettings.machine.floppyWriteProtected;
	for (int id = 0; id < 7; id++) {
		if (config.scsi[id].kind == bebox::MachineConfig::ScsiTarget::kCdRom
			&& fSettings.machine.scsi[id].kind
				== bebox::MachineConfig::ScsiTarget::kCdRom)
			config.scsi[id].image = fSettings.machine.scsi[id].image;
	}
	_ReplyToStart(_StartMachine(config) ? B_OK : B_ERROR);
	_UpdateMenus();
	_UpdateStatus();
}


// BeOS's changes go back to the folders (the machine has stopped).
void
MachineWindow::_SyncSharedFolders(bool interactive)
{
	if (fSharedFolders.empty())
		return;
	std::shared_ptr<std::vector<std::unique_ptr<bebox::SharedFolder> > >
		folders(new std::vector<std::unique_ptr<bebox::SharedFolder> >(
			std::move(fSharedFolders)));
	fSharedFolders.clear();
	fFolderSyncs++;
	BMessenger window(this);
	fFolderJobs.Add([folders, interactive, window]() {
		BString messages;
		for (auto& share : *folders) {
			std::string report;
			std::string error;
			if (!share->Sync(report, error))
				messages << error.c_str() << "\n\n";
			else if (!report.empty())
				messages << report.c_str() << "\n\n";
		}
		folders->clear();
		BMessage done(kMsgSharedFoldersSynced);
		if (interactive && messages.Length() > 0)
			done.AddString("report", messages.Trim());
		window.SendMessage(&done);
	});
}


void
MachineWindow::_ReplyToStart(status_t status)
{
	for (BMessage* message : fStartReplies) {
		BMessage reply(B_REPLY);
		if (status != B_OK) {
			reply.what = B_MESSAGE_NOT_UNDERSTOOD;
			reply.AddString("message", strerror(status));
		}
		reply.AddInt32("error", status);
		message->SendReply(&reply);
		delete message;
	}
	fStartReplies.clear();
}


void
MachineWindow::_EjectFloppy()
{
	fSettings.machine.floppyImage.clear();
	fEmulator.Post([](bebox::Machine& machine) {
		if (machine.Floppy() != nullptr)
			machine.Floppy()->EjectDisk(machine.FloppyUnit());
	});
	_MediaChanged();
	_UpdateMenus();
}


std::vector<int>
MachineWindow::_CdDrives() const
{
	std::vector<int> drives;
	for (int id = 0; id < 7; id++) {
		if (fSettings.machine.scsi[id].kind
				== bebox::MachineConfig::ScsiTarget::kCdRom)
			drives.push_back(id);
	}
	return drives;
}


void
MachineWindow::_ShowCdPanel(int id)
{
	if (fCdPanel == nullptr) {
		fCdPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this),
			nullptr, B_FILE_NODE, false);
	}
	BMessage message(kMsgCdChosen);
	message.AddInt32("id", id);
	fCdPanel->SetMessage(&message);
	BWindow* window = fCdPanel->Window();
	if (window->Lock()) {
		BString title;
		title.SetToFormat("%s: CD-ROM (SCSI ID %d)", kAppName, id);
		window->SetTitle(title.String());
		window->Unlock();
	}
	fCdPanel->Show();
}


bool
MachineWindow::_InsertCd(int id, const char* path, BString* errorText)
{
	std::string error;
	std::unique_ptr<bebox::CdImage> image = bebox::CdImage::Open(path,
		error);
	if (!image) {
		BString text;
		text.SetToFormat("The CD image could not be opened: %s",
			error.c_str());
		if (errorText != nullptr)
			*errorText = text;
		else
			(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
		return false;
	}
	fSettings.machine.scsi[id].image = path;
	_ChangeCd(id, std::move(image));
	return true;
}


void
MachineWindow::_EjectCd(int id)
{
	fSettings.machine.scsi[id].image.clear();
	_ChangeCd(id, nullptr);
}


// Into the running machine: a disc already in comes out first, the new
// one goes in a few seconds later (ScsiCd::Change()).
void
MachineWindow::_ChangeCd(int id, std::unique_ptr<bebox::CdImage> image)
{
	if (fPowered) {
		// commands are copied: share the image
		std::shared_ptr<std::unique_ptr<bebox::CdImage> > disc(
			new std::unique_ptr<bebox::CdImage>(std::move(image)));
		fEmulator.Post([id, disc](bebox::Machine& machine) {
			if (bebox::ScsiCd* cd = machine.CdRom(id))
				cd->Change(std::move(*disc));
		});
	}
	_MediaChanged();
	_UpdateMenus();
}


// Keeps an open preferences window's floppy and CD fields current.
void
MachineWindow::_MediaChanged()
{
	if (!fPrefsWindow.IsValid())
		return;
	BMessage message(kMsgMediaChanged);
	message.AddString("floppy", fSettings.machine.floppyImage.c_str());
	for (int id : _CdDrives()) {
		message.AddInt32("id", id);
		message.AddString("path", fSettings.machine.scsi[id].image.c_str());
	}
	fPrefsWindow.SendMessage(&message);
}


// Types ASCII text: each character a key press of 20 ms (with Shift for
// capitals and symbols), 20 ms apart.
void
MachineWindow::_TypeText(const char* text)
{
	std::vector<std::pair<bebox::u32, bool> > events;
	bebox::u32 shift = bebox::KeyNameToSet2("LeftShift");
	for (const char* c = text; *c != '\0'; c++) {
		bool shifted;
		bebox::u32 code = bebox::AsciiToSet2(*c, shifted);
		if (code == 0)
			continue;
		if (shifted)
			events.push_back(std::make_pair(shift, true));
		events.push_back(std::make_pair(code, true));
		events.push_back(std::make_pair(code, false));
		if (shifted)
			events.push_back(std::make_pair(shift, false));
	}
	fEmulator.Post([events](bebox::Machine& machine) {
		machine.Keyboard().TypeKeys(events,
			machine.MicrosecondsToTicks(20000));
	});
}


// Presses the keys in order and releases them in reverse, 50 ms apart.
void
MachineWindow::_PressKeyCombination(const std::vector<const char*>& keys)
{
	std::vector<std::pair<bebox::u32, bool> > events;
	for (const char* key : keys)
		events.push_back(std::make_pair(bebox::KeyNameToSet2(key), true));
	for (auto key = keys.rbegin(); key != keys.rend(); key++)
		events.push_back(std::make_pair(bebox::KeyNameToSet2(*key), false));
	fEmulator.Post([events](bebox::Machine& machine) {
		machine.Keyboard().TypeKeys(events,
			machine.MicrosecondsToTicks(50000));
	});
}


void
MachineWindow::FrameMoved(BPoint origin)
{
	if (!fFullScreen)
		fSettings.windowFrame = Frame();
	BWindow::FrameMoved(origin);
}


void
MachineWindow::FrameResized(float width, float height)
{
	if (!fFullScreen)
		fSettings.windowFrame = Frame();
	BWindow::FrameResized(width, height);
}


void
MachineWindow::_ShowConsole(int port)
{
	if (port < 0 || port > 3)
		return;
	if (fSettings.serial[port] != "console") {
		BString text;
		text.SetToFormat("Serial port %d is not set to a console window. "
			"Change it in the preferences (Serial ports).", port + 1);
		(new BAlert(kAppName, text.String(), "OK"))->Go(nullptr);
		return;
	}
	if (fConsoleWindows[port].IsValid()) {
		BLooper* looper;
		BWindow* window = dynamic_cast<BWindow*>(
			fConsoleWindows[port].Target(&looper));
		if (window != nullptr && window->Lock()) {
			window->Activate();
			window->Unlock();
			return;
		}
	}
	SerialConsoleWindow* window = new SerialConsoleWindow(fConsoles[port]);
	fConsoleWindows[port] = BMessenger(window);
	window->Show();
}


bool
MachineWindow::QuitRequested()
{
	PowerOff();
	// BeOS's changes are in the shared folders before the app goes
	fFolderJobs.Wait();
	fSettings.windowFrame = fFullScreen ? fWindowedFrame : Frame();
	fSettings.Save();
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}
