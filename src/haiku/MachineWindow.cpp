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
#include "core/storage/DiskImage.h"

#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <Directory.h>
#include <File.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <Joystick.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Path.h>
#include <PropertyInfo.h>
#include <StringList.h>
#include <StringView.h>
#include <TranslatorRoster.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>

static const uint32 kMsgTick = 'tick';
static const uint32 kMsgJoystickPoll = 'joyp';
static const uint32 kMsgHeldKey = 'hold';
// The host key toggles the mouse capture and is not passed to the BeBox
// (key code, modifier bit or 0), by Settings::hostKey.
static const struct {
	uint32 key;
	uint32 modifier;
} kHostKeys[4] = {
	{ 0x60, B_RIGHT_CONTROL_KEY },
	{ 0x5f, B_RIGHT_COMMAND_KEY },		// right Alt
	{ 0x67, B_RIGHT_OPTION_KEY },		// right Windows key
	{ 0x68, 0 }							// Menu
};


MachineWindow::MachineWindow(Settings& settings)
	:
	BWindow(settings.windowFrame, kAppName, B_TITLED_WINDOW,
		B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS),
	fSettings(settings),
	fConsoleMenu(nullptr),
	fFloppyPanel(nullptr),
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
	fLeftLeds = new LedColumnView("cpu0 leds", false);
	fRightLeds = new LedColumnView("cpu1 leds", true);
	fLeftLeds->SetStyle(settings.ledStyle);
	fRightLeds->SetStyle(settings.ledStyle);
	fStatus = new BStringView("status", "Powered off");
	fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fMedia = new MediaStatusView();

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(menuBar)
		.AddGroup(B_HORIZONTAL, 0)
			.Add(fLeftLeds)
			.Add(fScreen)
			.Add(fRightLeds)
		.End()
		.AddGroup(B_HORIZONTAL, 0)
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
	bool wasPowered = fPowered;
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
	return menu;
}


BMenu*
MachineWindow::_BuildViewMenu()
{
	BMenu* menu = new BMenu("View");
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
	if (fPowered)
		return true;
	std::string error;
	bebox::MachineConfig config = fSettings.MachineConfigForRun();
	_PrepareSharedFolders(config);
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
	HaikuAudio* audio = nullptr;
	if (fSettings.soundEnabled) {
		fAudio.SetVolume(fSettings.soundVolume / 100.0f);
		if (fAudio.Start() == B_OK)
			audio = &fAudio;
	}
	if (!fEmulator.Start(config, error,
			[screen, audio, serial, network](bebox::Machine& machine) {
				machine.SetVideoSink(screen);
				machine.SetAudioSink(audio);
				if (network != nullptr)
					machine.SetNetworkBackend(network);
				for (int i = 0; i < 4; i++) {
					if (serial[i] != nullptr)
						machine.SetSerialBackend(i, serial[i]);
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
	fPowerItem->SetLabel(fPowered ? "Power off" : "Power on");
	fPowerItem->Message()->what = fPowered ? kMsgPowerOff : kMsgPowerOn;
	fPauseItem->SetMarked(fEmulator.IsPaused());
	fCtrlAltDeleteItem->SetEnabled(fPowered);
	for (int32 i = 0; i < 3; i++)
		fScaleItems[i]->SetMarked(fSettings.scale == (i == 0 ? 0 : i));
	fShowLedsItem->SetMarked(fSettings.showLeds);
	// the Hobbit prototype's kernel drives no front panel lights
	fShowLedsItem->SetEnabled(
		fSettings.machine.model != bebox::MachineConfig::kHobbit);
	fEjectFloppyItem->SetEnabled(!fSettings.machine.floppyImage.empty());
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
	if (show != fLeftLeds->IsHidden())
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
	if (!fPowered) {
		text.SetToFormat("%s  \xe2\x80\x94  powered off",
			fSettings.profile.String());
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
		static const char* const kHostKeyNames[4] = { "right Ctrl",
			"right Alt", "right Windows key", "Menu key" };
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
	if (fSettings.scale == 0)
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
		{ B_LEFT_CONTROL_KEY, 0x5c },
		{ B_LEFT_COMMAND_KEY, 0x5d }, { B_RIGHT_COMMAND_KEY, 0x5f },
		{ B_LEFT_OPTION_KEY, 0x66 }, { B_RIGHT_OPTION_KEY, 0x67 },
		{ B_MENU_KEY, 0x68 }
	};
	for (const Modifier& modifier : kModifiers) {
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
	// A modifier host key toggles the mouse capture when released.
	uint32 modifier = kHostKeys[_HostKeyIndex()].modifier;
	if (modifier != 0 && (old & modifier) != 0 && (now & modifier) == 0)
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
				if (kHostKeys[_HostKeyIndex()].modifier == 0)
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


void
MachineWindow::DispatchMessage(BMessage* message, BHandler* handler)
{
	if (fPowered && IsActive()) {
		switch (message->what) {
			case B_KEY_DOWN:
			case B_KEY_UP:
			case B_UNMAPPED_KEY_DOWN:
			case B_UNMAPPED_KEY_UP:
			case B_MODIFIERS_CHANGED:
				if (_HandleKey(message))
					return;
				break;
		}
	}
	BWindow::DispatchMessage(message, handler);
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
			break;
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
			fEmulator.Post([](bebox::Machine& machine) {
				machine.PowerOn();
			});
			// DR7/DR8 ROMs: any key; PR and later: left Shift. F1 sends
			// the kernel's debug output to serial port 4.
			// F1 must be up again before the ROM looks for keys that
			// open its boot menu (about 6 s after power on).
			bool debug = message->what == kMsgBootWithDebug;
			_HoldKeyDuringBoot(debug ? 0x05 : 0x12, debug ? 5.5 : 8);
			if (debug && fSettings.serial[3] == "console")
				_ShowConsole(3);
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
			bool wasPowered = fPowered;
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
			bool restart = message->GetBool("restart", false) && fPowered;
			if (restart)
				PowerOff();
			if (message->GetBool("reset_flash", false))
				unlink(fSettings.FlashPath().String());
			if (restart)
				PowerOn();
			else if (fPowered)
				_OpenJoysticks();
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
			else if (on && !fPowered)
				result = PowerOn() ? B_OK : B_ERROR;
			else if (!on && fPowered)
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
	} else
		return false;
	if (result != B_OK) {
		reply.what = B_MESSAGE_NOT_UNDERSTOOD;
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


// Shared folders: a BFS volume per folder, built at power on in the
// settings folder; BeOS's changes go back to the folder at power off.
void
MachineWindow::_PrepareSharedFolders(bebox::MachineConfig& config)
{
	fSharedFolders.clear();
	BString messages;
	for (int id = 0; id < 7; id++) {
		bebox::MachineConfig::ScsiTarget& target = config.scsi[id];
		if (target.kind != bebox::MachineConfig::ScsiTarget::kSharedFolder)
			continue;
		BString image = fSettings.ProfileDirectory();
		image << "/shared-scsi" << id << ".qcow2";
		std::unique_ptr<bebox::SharedFolder> share(new bebox::SharedFolder(
			target.image, image.String(), &fHostAttributes));
		std::string report;
		std::string error;
		if (!share->Prepare(report, error)) {
			messages << error.c_str() << "\n\n";
			target.kind = bebox::MachineConfig::ScsiTarget::kNone;
			continue;
		}
		if (!report.empty())
			messages << report.c_str() << "\n\n";
		target.image = image.String();
		target.readOnly = false;
		fSharedFolders.push_back(std::move(share));
	}
	if (messages.Length() > 0) {
		messages.Trim();
		(new BAlert(kAppName, messages.String(), "OK"))->Go(nullptr);
	}
}


void
MachineWindow::_SyncSharedFolders(bool interactive)
{
	BString messages;
	for (auto& share : fSharedFolders) {
		std::string report;
		std::string error;
		if (!share->Sync(report, error))
			messages << error.c_str() << "\n\n";
		else if (!report.empty())
			messages << report.c_str() << "\n\n";
	}
	fSharedFolders.clear();
	if (interactive && messages.Length() > 0) {
		messages.Trim();
		(new BAlert(kAppName, messages.String(), "OK"))->Go(nullptr);
	}
}


void
MachineWindow::_EjectFloppy()
{
	fSettings.machine.floppyImage.clear();
	fEmulator.Post([](bebox::Machine& machine) {
		if (machine.Floppy() != nullptr)
			machine.Floppy()->EjectDisk(machine.FloppyUnit());
	});
	_UpdateMenus();
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
	fSettings.windowFrame = Frame();
	BWindow::FrameMoved(origin);
}


void
MachineWindow::FrameResized(float width, float height)
{
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
	fSettings.windowFrame = Frame();
	fSettings.Save();
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}
