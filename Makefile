## Haiku Generic Makefile v2.6 ##
## BeBox emulator: build natively on Haiku (needs haiku_devel): make

NAME = BeBox
TYPE = APP
APP_MIME_SIG = application/x-vnd.BeBox-Emulator

# The portable emulator core (also built and tested on Linux, see
# tests/linux) and the Haiku front end.
SRCS = \
	$(wildcard src/core/*.cpp) \
	$(wildcard src/core/*/*.cpp) \
	src/haiku/App.cpp \
	src/haiku/HaikuAttributes.cpp \
	src/haiku/HaikuAudio.cpp \
	src/haiku/LedColumnView.cpp \
	src/haiku/MachineWindow.cpp \
	src/haiku/MediaStatusView.cpp \
	src/haiku/NewDiskWindow.cpp \
	src/haiku/PrefsWindow.cpp \
	src/haiku/ProfileNameWindow.cpp \
	src/haiku/ScreenView.cpp \
	src/haiku/SerialConsole.cpp \
	src/haiku/Settings.cpp

RDEFS = BeBox.rdef

# game: set_mouse_position(). tracker: BFilePanel. translation: screenshots.
# media: BSoundPlayer. device: BJoystick. z: compressed QCOW2 clusters.
# slirp: user mode networking (libslirp_devel).
LIBS = be device game media network tracker translation slirp z \
	$(STDCPPLIBS)
DEFINES = HAVE_SLIRP

LOCAL_INCLUDE_PATHS = src src/haiku
SYSTEM_INCLUDE_PATHS = /boot/system/develop/headers/private/shared \
	/boot/system/develop/headers/private/interface
OPTIMIZE := FULL
WARNINGS = ALL
COMPILER_FLAGS = -std=c++17
DEBUGGER :=

DEVEL_DIRECTORY := /boot/system/develop
include $(DEVEL_DIRECTORY)/etc/makefile-engine
