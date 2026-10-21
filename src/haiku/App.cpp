/*
 * BeBox emulator for Haiku - the application.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Command line (also delivered to a running instance):
 *   BeBox [--profile NAME]
 *         [--rom-dir DIR] [--boot-rom IMAGE] [--model 66|133] [--ram MB]
 *         [--video s3|cirrus] [--net] [--net-forward SPEC] [--ide0 FILE]
 *         [--ide1 FILE] [--scsiN-disk FILE] [--scsiN-cd FILE]
 *         [--scsiN-share FOLDER]
 *         [--floppy FILE] [--power-on]
 * (N is the SCSI ID, 0-6.) --profile selects a machine profile (made as a
 * copy of the current one if there is none); the other options change
 * that profile.
 */
#include "BeBox.h"
#include "MachineWindow.h"
#include "Settings.h"

#include <AboutWindow.h>
#include <Application.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

class App : public BApplication {
public:
	App()
		:
		BApplication(kAppSignature),
		fWindow(nullptr),
		fPowerOn(false)
	{
		fSettings.Load();
	}

	virtual void ArgvReceived(int32 argc, char** argv)
	{
		for (int32 i = 1; i + 1 < argc; i++) {
			if (strcmp(argv[i], "--profile") == 0)
				_SelectProfile(argv[i + 1]);
		}
		BString romDirectory = fSettings.romDirectory;
		BString bootRomImage = fSettings.bootRomImage;
		for (int32 i = 1; i < argc; i++) {
			const char* arg = argv[i];
			const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
			if (strcmp(arg, "--power-on") == 0) {
				fPowerOn = true;
				continue;
			}
			if (strcmp(arg, "--net") == 0) {
				fSettings.ethernet = true;
				continue;
			}
			if (value == nullptr)
				break;
			if (strcmp(arg, "--profile") == 0)
				;	// done first
			else if (strcmp(arg, "--rom-dir") == 0)
				fSettings.romDirectory = value;
			else if (strcmp(arg, "--boot-rom") == 0)
				fSettings.bootRomImage = value;
			else if (strcmp(arg, "--model") == 0) {
				fSettings.machine.model = strcmp(value, "133") == 0
					? bebox::MachineConfig::kBeBox133
					: strcmp(value, "hobbit") == 0
					? bebox::MachineConfig::kHobbit
					: bebox::MachineConfig::kBeBox66;
			} else if (strcmp(arg, "--kernel") == 0)
				fSettings.machine.hobbitKernelPath = value;
			else if (strcmp(arg, "--ram") == 0)
				fSettings.machine.ramMB = atoi(value);
			else if (strcmp(arg, "--net-forward") == 0) {
				fSettings.ethernet = true;
				fSettings.portForwards = value;
			} else if (strcmp(arg, "--video") == 0)
				fSettings.machine.videoCard = strcmp(value, "cirrus") == 0
					? bebox::MachineConfig::kVideoCirrus5434
					: bebox::MachineConfig::kVideoS3Vision864;
			else if (strcmp(arg, "--ide0") == 0)
				fSettings.machine.ideImage[0] = value;
			else if (strcmp(arg, "--ide1") == 0)
				fSettings.machine.ideImage[1] = value;
			else if (strcmp(arg, "--floppy") == 0)
				fSettings.machine.floppyImage = value;
			else if (strncmp(arg, "--scsi", 6) == 0 && arg[6] >= '0'
				&& arg[6] <= '6' && (strcmp(arg + 7, "-disk") == 0
					|| strcmp(arg + 7, "-cd") == 0
					|| strcmp(arg + 7, "-share") == 0)) {
				bebox::MachineConfig::ScsiTarget& target
					= fSettings.machine.scsi[arg[6] - '0'];
				// an empty image removes the device (a CD-ROM drive stays,
				// empty)
				bool cd = strcmp(arg + 7, "-cd") == 0;
				target.kind = cd ? bebox::MachineConfig::ScsiTarget::kCdRom
					: value[0] == '\0'
						? bebox::MachineConfig::ScsiTarget::kNone
					: strcmp(arg + 7, "-share") == 0
						? bebox::MachineConfig::ScsiTarget::kSharedFolder
						: bebox::MachineConfig::ScsiTarget::kDisk;
				target.image = value;
				target.readOnly = target.kind
					== bebox::MachineConfig::ScsiTarget::kCdRom;
			} else
				continue;
			i++;
		}
		// the flash holds the previous boot ROM: replace it
		if (fSettings.romDirectory != romDirectory
			|| fSettings.bootRomImage != bootRomImage)
			unlink(fSettings.FlashPath().String());
		if (fWindow != nullptr && fPowerOn)
			fWindow->PostMessage(kMsgPowerOn);
	}

	virtual void ReadyToRun()
	{
		fWindow = new MachineWindow(fSettings);
		fWindow->Show();
		if (fPowerOn)
			fWindow->PostMessage(kMsgPowerOn);
	}

	virtual void AboutRequested()
	{
		BAboutWindow* about = new BAboutWindow(kAppName, kAppSignature);
		about->AddDescription("\"BeBox\" is a PowerPC and Hobbit BeBox "
			"emulator written in C++ for the Haiku Operating System.");
		const char* authors[] = { "Claude Opus 5.5", "John Rash", nullptr };
		about->AddAuthors(authors);
		about->AddCopyright(2026, "The BeBox emulator authors");
		// one block: no blank line between the two
		about->AddText("Distributed under the terms of the MIT License.\n"
			"BeBox icon by zuMi, used under the MIT License.");
		about->Show();
	}

private:
	void _SelectProfile(const char* name)
	{
		if (fWindow != nullptr) {
			if (fWindow->Lock()) {
				fWindow->SwitchProfile(name, true);
				fWindow->Unlock();
			}
			return;
		}
		status_t status = B_OK;
		if (Settings::ProfileExists(name))
			status = fSettings.SwitchProfile(name);
		else if (Settings::CheckProfileName(name).IsEmpty())
			status = fSettings.CopyProfile(name);
		else
			status = B_BAD_VALUE;
		if (status != B_OK)
			fprintf(stderr, "BeBox: cannot use the profile \"%s\"\n", name);
	}

	Settings		fSettings;
	MachineWindow*	fWindow;
	bool			fPowerOn;
};


int
main()
{
	App app;
	app.Run();
	return 0;
}
