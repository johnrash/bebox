/*
 * BeBox emulator for Haiku - persistent settings and machine profiles.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Settings.h"

#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <OS.h>
#include <Path.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

using bebox::MachineConfig;

static const char* const kDefaultProfile = "Default";
// The per profile files kept in the settings folder before profiles.
static const char* const kLegacyFiles[] = { "flash.bin", "nvram.bin" };
static const char* const kLegacySharePrefix = "shared-scsi";


static status_t
CopyFile(const char* source, const char* target)
{
	BFile in(source, B_READ_ONLY);
	status_t status = in.InitCheck();
	if (status != B_OK)
		return status;
	BFile out(target, B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status = out.InitCheck();
	if (status != B_OK)
		return status;
	char buffer[65536];
	ssize_t read;
	while ((read = in.Read(buffer, sizeof(buffer))) > 0) {
		if (out.Write(buffer, read) != read)
			return B_IO_ERROR;
	}
	return read < 0 ? (status_t)read : B_OK;
}


static status_t
ReadArchive(const char* path, BMessage& archive)
{
	BFile file(path, B_READ_ONLY);
	status_t status = file.InitCheck();
	if (status != B_OK)
		return status;
	return archive.Unflatten(&file);
}


static status_t
WriteArchive(const char* path, const BMessage& archive)
{
	BFile file(path, B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status_t status = file.InitCheck();
	if (status != B_OK)
		return status;
	return archive.Flatten(&file);
}


Settings::Settings()
	:
	scale(1),
	showLeds(true),
	ledStyle(0),
	hostKey(kHostKeyRightControl),
	mouseSpeed(100),
	swapMouseButtons(false),
	captureOnClick(true),
	windowFrame(80, 80, 80 + 700, 80 + 560)
{
	srandom((unsigned)system_time());
	_ResetProfile();
}


BString
Settings::Directory()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path, true) != B_OK)
		path.SetTo("/boot/home/config/settings");
	path.Append("BeBox");
	create_directory(path.Path(), 0755);
	return BString(path.Path());
}


BString
Settings::ProfilesDirectory()
{
	BString directory = Directory();
	directory << "/Profiles";
	create_directory(directory.String(), 0755);
	return directory;
}


BString
Settings::ProfileDirectory() const
{
	BString directory = ProfilesDirectory();
	directory << "/" << profile;
	return directory;
}


// #pragma mark - profile settings


void
Settings::_ResetProfile()
{
	machine = MachineConfig();
	machine.threadPerCpu = true;
	romDirectory = Directory();
	romDirectory << "/ROMs";
	bootRomImage = "";
	speedLimit = 100;
	soundEnabled = true;
	soundVolume = 100;
	for (int i = 0; i < 4; i++) {
		serial[i] = "";
		joystick[i] = "";
	}
	serial[3] = "console";		// the kernel's debug output
	ethernet = false;
	ethernetPort = 0x300;
	ethernetIrq = 5;
	// a locally made up address after the NE2000 clone prefix
	macAddress.SetToFormat("00:00:e8:%02x:%02x:%02x", (int)(random() & 0xff),
		(int)(random() & 0xff), (int)(random() & 0xff));
	portForwards = "";
}


void
Settings::_ReadProfile(const BMessage& archive)
{
	int32 value;
	bool flag;
	BString string;
	if (archive.FindInt32("model", &value) == B_OK) {
		machine.model = value == 133 ? MachineConfig::kBeBox133
			: value == 25 ? MachineConfig::kHobbit : MachineConfig::kBeBox66;
	}
	if (archive.FindInt32("ram", &value) == B_OK)
		machine.ramMB = value;
	if (archive.FindInt32("cpus", &value) == B_OK)
		machine.cpuCount = value;
	if (archive.FindInt32("board_revision", &value) == B_OK)
		machine.boardRevision = value;
	if (archive.FindInt32("io_card_revision", &value) == B_OK)
		machine.ioCardRevision = value;
	if (archive.FindInt32("video", &value) == B_OK)
		machine.videoCard = (MachineConfig::VideoCard)value;
	if (archive.FindInt32("vram", &value) == B_OK)
		machine.vramMB = value;
	if (archive.FindInt32("quantum", &value) == B_OK)
		machine.quantumUs = value;
	if (archive.FindInt32("cpi_percent", &value) == B_OK)
		machine.cpiPercent = value;
	if (archive.FindBool("thread_per_cpu", &flag) == B_OK)
		machine.threadPerCpu = flag;
	if (archive.FindString("rom_directory", &string) == B_OK)
		romDirectory = string;
	if (archive.FindString("boot_rom_image", &string) == B_OK)
		bootRomImage = string;
	if (archive.FindString("hobbit_kernel", &string) == B_OK)
		machine.hobbitKernelPath = string.String();
	for (int i = 0; i < 2; i++) {
		BString name;
		name.SetToFormat("ide%d", i);
		if (archive.FindString(name.String(), &string) == B_OK)
			machine.ideImage[i] = string.String();
		name << "_read_only";
		if (archive.FindBool(name.String(), &flag) == B_OK)
			machine.ideReadOnly[i] = flag;
	}
	for (int id = 0; id < 7; id++) {
		MachineConfig::ScsiTarget& target = machine.scsi[id];
		BString name;
		name.SetToFormat("scsi%d_kind", id);
		if (archive.FindInt32(name.String(), &value) == B_OK)
			target.kind = (MachineConfig::ScsiTarget::Kind)value;
		name.SetToFormat("scsi%d_image", id);
		if (archive.FindString(name.String(), &string) == B_OK)
			target.image = string.String();
		name.SetToFormat("scsi%d_read_only", id);
		if (archive.FindBool(name.String(), &flag) == B_OK)
			target.readOnly = flag;
	}
	if (archive.FindString("floppy", &string) == B_OK)
		machine.floppyImage = string.String();
	if (archive.FindBool("floppy_write_protected", &flag) == B_OK)
		machine.floppyWriteProtected = flag;
	DropEmptyDevices(machine);
	if (archive.FindInt32("speed_limit", &value) == B_OK)
		speedLimit = value;
	if (archive.FindBool("sound", &flag) == B_OK)
		soundEnabled = flag;
	if (archive.FindInt32("sound_volume", &value) == B_OK)
		soundVolume = value;
	for (int i = 0; i < 4; i++) {
		BString name;
		name.SetToFormat("serial%d", i + 1);
		if (archive.FindString(name.String(), &string) == B_OK)
			serial[i] = string;
		name.SetToFormat("joystick%d", i + 1);
		if (archive.FindString(name.String(), &string) == B_OK)
			joystick[i] = string;
	}
	if (archive.FindBool("ethernet", &flag) == B_OK)
		ethernet = flag;
	if (archive.FindInt32("ethernet_port", &value) == B_OK)
		ethernetPort = value;
	if (archive.FindInt32("ethernet_irq", &value) == B_OK)
		ethernetIrq = value;
	if (archive.FindString("mac_address", &string) == B_OK)
		macAddress = string;
	if (archive.FindString("port_forwards", &string) == B_OK)
		portForwards = string;
}


void
Settings::_WriteProfile(BMessage& archive) const
{
	// the processors' clock (MHz)
	archive.AddInt32("model",
		machine.model == MachineConfig::kBeBox133 ? 133
			: machine.model == MachineConfig::kHobbit ? 25 : 66);
	archive.AddInt32("ram", machine.ramMB);
	archive.AddInt32("cpus", machine.cpuCount);
	archive.AddInt32("board_revision", machine.boardRevision);
	archive.AddInt32("io_card_revision", machine.ioCardRevision);
	archive.AddInt32("video", machine.videoCard);
	archive.AddInt32("vram", machine.vramMB);
	archive.AddInt32("quantum", machine.quantumUs);
	archive.AddInt32("cpi_percent", machine.cpiPercent);
	archive.AddBool("thread_per_cpu", machine.threadPerCpu);
	archive.AddString("rom_directory", romDirectory);
	archive.AddString("boot_rom_image", bootRomImage);
	archive.AddString("hobbit_kernel", machine.hobbitKernelPath.c_str());
	for (int i = 0; i < 2; i++) {
		BString name;
		name.SetToFormat("ide%d", i);
		archive.AddString(name.String(), machine.ideImage[i].c_str());
		name << "_read_only";
		archive.AddBool(name.String(), machine.ideReadOnly[i]);
	}
	for (int id = 0; id < 7; id++) {
		const MachineConfig::ScsiTarget& target = machine.scsi[id];
		BString name;
		name.SetToFormat("scsi%d_kind", id);
		archive.AddInt32(name.String(), target.kind);
		name.SetToFormat("scsi%d_image", id);
		archive.AddString(name.String(), target.image.c_str());
		name.SetToFormat("scsi%d_read_only", id);
		archive.AddBool(name.String(), target.readOnly);
	}
	archive.AddString("floppy", machine.floppyImage.c_str());
	archive.AddBool("floppy_write_protected", machine.floppyWriteProtected);
	archive.AddInt32("speed_limit", speedLimit);
	archive.AddBool("sound", soundEnabled);
	archive.AddInt32("sound_volume", soundVolume);
	for (int i = 0; i < 4; i++) {
		BString name;
		name.SetToFormat("serial%d", i + 1);
		archive.AddString(name.String(), serial[i]);
		name.SetToFormat("joystick%d", i + 1);
		archive.AddString(name.String(), joystick[i]);
	}
	archive.AddBool("ethernet", ethernet);
	archive.AddInt32("ethernet_port", ethernetPort);
	archive.AddInt32("ethernet_irq", ethernetIrq);
	archive.AddString("mac_address", macAddress);
	archive.AddString("port_forwards", portForwards);
}


status_t
Settings::_LoadProfile()
{
	_ResetProfile();
	BMessage archive;
	BString path = ProfileDirectory();
	path << "/settings";
	status_t status = ReadArchive(path.String(), archive);
	if (status == B_OK)
		_ReadProfile(archive);
	return status;
}


status_t
Settings::_SaveProfile() const
{
	BString directory = ProfileDirectory();
	create_directory(directory.String(), 0755);
	BMessage archive('BBxp');
	_WriteProfile(archive);
	directory << "/settings";
	return WriteArchive(directory.String(), archive);
}


// Settings from before profiles (the machine in "settings", the flash,
// NVRAM and shared folder volumes next to it) become the profile
// "Default".
void
Settings::_Migrate(const BMessage& archive)
{
	_ResetProfile();
	_ReadProfile(archive);
	profile = kDefaultProfile;
	BString target = ProfileDirectory();
	create_directory(target.String(), 0755);
	BDirectory directory(Directory().String());
	BEntry entry;
	while (directory.GetNextEntry(&entry) == B_OK) {
		char name[B_FILE_NAME_LENGTH];
		if (entry.GetName(name) != B_OK || entry.IsDirectory())
			continue;
		bool move = BString(name).StartsWith(kLegacySharePrefix);
		for (const char* legacy : kLegacyFiles)
			move |= strcmp(name, legacy) == 0;
		if (move) {
			BDirectory profileDirectory(target.String());
			entry.MoveTo(&profileDirectory);
		}
	}
	_SaveProfile();
}


// #pragma mark -


status_t
Settings::Load()
{
	BString path = Directory();
	path << "/settings";
	BMessage archive;
	ReadArchive(path.String(), archive);

	int32 value;
	bool flag;
	BString string;
	if (archive.FindInt32("scale", &value) == B_OK)
		scale = value;
	if (archive.FindBool("show_leds", &flag) == B_OK)
		showLeds = flag;
	if (archive.FindInt32("led_style", &value) == B_OK)
		ledStyle = value;
	if (archive.FindInt32("host_key", &value) == B_OK)
		hostKey = value;
	if (archive.FindInt32("mouse_speed", &value) == B_OK)
		mouseSpeed = value;
	if (archive.FindBool("swap_mouse_buttons", &flag) == B_OK)
		swapMouseButtons = flag;
	if (archive.FindBool("capture_on_click", &flag) == B_OK)
		captureOnClick = flag;
	BRect frame;
	if (archive.FindRect("window_frame", &frame) == B_OK)
		windowFrame = frame;

	BStringList profiles;
	GetProfiles(profiles);
	if (profiles.IsEmpty()) {
		_Migrate(archive);
		return Save();
	}
	if (archive.FindString("profile", &string) == B_OK
		&& ProfileExists(string.String()))
		profile = string;
	else
		profile = profiles.StringAt(0);
	return _LoadProfile();
}


status_t
Settings::Save() const
{
	BMessage archive('BBxs');
	archive.AddString("profile", profile);
	archive.AddInt32("scale", scale);
	archive.AddBool("show_leds", showLeds);
	archive.AddInt32("led_style", ledStyle);
	archive.AddInt32("host_key", hostKey);
	archive.AddInt32("mouse_speed", mouseSpeed);
	archive.AddBool("swap_mouse_buttons", swapMouseButtons);
	archive.AddBool("capture_on_click", captureOnClick);
	archive.AddRect("window_frame", windowFrame);

	BString path = Directory();
	path << "/settings";
	status_t status = WriteArchive(path.String(), archive);
	if (status != B_OK)
		return status;
	return _SaveProfile();
}


MachineConfig
Settings::MachineConfigForRun() const
{
	MachineConfig config = machine;
	DropEmptyDevices(config);
	config.nubRomPath = BString(romDirectory).Append("/bootnub.rom").String();
	config.mainRomPath = bootRomImage.IsEmpty()
		? BString(romDirectory).Append("/bootmain.rom").String()
		: bootRomImage.String();
	config.flashPath = FlashPath().String();
	config.nvramPath = NvramPath().String();
	config.ethernet = ethernet;
	config.ethernetPort = (bebox::u32)ethernetPort;
	config.ethernetIrq = ethernetIrq;
	unsigned int mac[6];
	if (sscanf(macAddress.String(), "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1],
			&mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
		for (int i = 0; i < 6; i++)
			config.ethernetMac[i] = (bebox::u8)mac[i];
	}
	return config;
}


BString
Settings::FlashPath() const
{
	return ProfileDirectory().Append("/flash.bin");
}


BString
Settings::NvramPath() const
{
	return ProfileDirectory().Append("/nvram.bin");
}


void
Settings::DropEmptyDevices(MachineConfig& machine)
{
	for (int id = 0; id < 7; id++) {
		MachineConfig::ScsiTarget& target = machine.scsi[id];
		BString image(target.image.c_str());
		if (image.Trim().IsEmpty()) {
			target.kind = MachineConfig::ScsiTarget::kNone;
			target.image.clear();
		}
	}
	for (int i = 0; i < 2; i++) {
		BString image(machine.ideImage[i].c_str());
		if (image.Trim().IsEmpty())
			machine.ideImage[i].clear();
	}
	BString floppy(machine.floppyImage.c_str());
	if (floppy.Trim().IsEmpty())
		machine.floppyImage.clear();
}


// #pragma mark - profiles


// A profile is a folder in Profiles/ with a settings file (a folder
// without one, say with an NVRAM image put there for a new profile, is not
// a profile yet).
void
Settings::GetProfiles(BStringList& names)
{
	names.MakeEmpty();
	BDirectory directory(ProfilesDirectory().String());
	BEntry entry;
	while (directory.GetNextEntry(&entry) == B_OK) {
		char name[B_FILE_NAME_LENGTH];
		if (entry.IsDirectory() && entry.GetName(name) == B_OK
			&& ProfileExists(name))
			names.Add(name);
	}
	names.Sort(true);
}


bool
Settings::ProfileExists(const char* name)
{
	if (name == nullptr || name[0] == '\0')
		return false;
	BString path = ProfilesDirectory();
	path << "/" << name << "/settings";
	return BEntry(path.String()).IsFile();
}


BString
Settings::CheckProfileName(const char* name)
{
	BString trimmed(name);
	trimmed.Trim();
	if (trimmed.IsEmpty())
		return "Enter a name for the profile.";
	if (trimmed != name)
		return "The name cannot begin or end with spaces.";
	if (trimmed.FindFirst('/') >= 0 || trimmed.StartsWith("."))
		return "The name cannot contain \"/\" or begin with a period.";
	if (trimmed.Length() >= B_FILE_NAME_LENGTH)
		return "The name is too long.";
	if (ProfileExists(name))
		return "There is a profile with that name already.";
	BString path = ProfilesDirectory();
	path << "/" << name;
	BEntry entry(path.String());
	if (entry.Exists() && !entry.IsDirectory())
		return "There is a file with that name in the profiles folder.";
	return "";
}


status_t
Settings::SwitchProfile(const char* name)
{
	if (!ProfileExists(name))
		return B_ENTRY_NOT_FOUND;
	status_t status = Save();
	if (status != B_OK)
		return status;
	profile = name;
	_LoadProfile();
	return Save();
}


status_t
Settings::CopyProfile(const char* name)
{
	BString source = ProfileDirectory();
	BString target = ProfilesDirectory();
	target << "/" << name;
	status_t status = create_directory(target.String(), 0755);
	if (status != B_OK)
		return status;
	for (const char* file : kLegacyFiles) {
		BString from = source;
		from << "/" << file;
		BString to = target;
		to << "/" << file;
		if (!BEntry(to.String()).Exists() && BEntry(from.String()).Exists()) {
			status = CopyFile(from.String(), to.String());
			if (status != B_OK)
				return status;
		}
	}
	profile = name;
	return Save();
}


status_t
Settings::RenameProfile(const char* name)
{
	BEntry entry(ProfileDirectory().String());
	status_t status = entry.Rename(name);
	if (status != B_OK)
		return status;
	profile = name;
	return Save();
}


status_t
Settings::DeleteProfile(const char* name)
{
	if (!ProfileExists(name))
		return B_ENTRY_NOT_FOUND;
	BString path = ProfilesDirectory();
	path << "/" << name;
	BDirectory directory(path.String());
	BEntry entry;
	std::vector<BEntry> entries;
	while (directory.GetNextEntry(&entry) == B_OK)
		entries.push_back(entry);
	for (BEntry& file : entries) {
		status_t status = file.Remove();
		if (status != B_OK)
			return status;
	}
	return BEntry(path.String()).Remove();
}
