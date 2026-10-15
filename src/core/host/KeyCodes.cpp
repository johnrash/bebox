/*
 * BeBox emulator - keyboard codes.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "KeyCodes.h"

#include <strings.h>

namespace bebox {

namespace {

struct Key {
	u8 haiku;
	u32 set2;
	const char* name;
};

const Key kKeys[] = {
	{ 0x01, 0x76, "Escape" },
	{ 0x02, 0x05, "F1" }, { 0x03, 0x06, "F2" }, { 0x04, 0x04, "F3" },
	{ 0x05, 0x0c, "F4" }, { 0x06, 0x03, "F5" }, { 0x07, 0x0b, "F6" },
	{ 0x08, 0x83, "F7" }, { 0x09, 0x0a, "F8" }, { 0x0a, 0x01, "F9" },
	{ 0x0b, 0x09, "F10" }, { 0x0c, 0x78, "F11" }, { 0x0d, 0x07, "F12" },
	{ 0x0e, 0xe07c, "PrintScreen" }, { 0x0f, 0x7e, "ScrollLock" },
	{ 0x10, 0xe11477, "Pause" },
	{ 0x11, 0x0e, "Grave" },
	{ 0x12, 0x16, "1" }, { 0x13, 0x1e, "2" }, { 0x14, 0x26, "3" },
	{ 0x15, 0x25, "4" }, { 0x16, 0x2e, "5" }, { 0x17, 0x36, "6" },
	{ 0x18, 0x3d, "7" }, { 0x19, 0x3e, "8" }, { 0x1a, 0x46, "9" },
	{ 0x1b, 0x45, "0" }, { 0x1c, 0x4e, "Minus" }, { 0x1d, 0x55, "Equal" },
	{ 0x1e, 0x66, "Backspace" },
	{ 0x1f, 0xe070, "Insert" }, { 0x20, 0xe06c, "Home" },
	{ 0x21, 0xe07d, "PageUp" },
	{ 0x22, 0x77, "NumLock" }, { 0x23, 0xe04a, "KeypadSlash" },
	{ 0x24, 0x7c, "KeypadStar" }, { 0x25, 0x7b, "KeypadMinus" },
	{ 0x26, 0x0d, "Tab" },
	{ 0x27, 0x15, "Q" }, { 0x28, 0x1d, "W" }, { 0x29, 0x24, "E" },
	{ 0x2a, 0x2d, "R" }, { 0x2b, 0x2c, "T" }, { 0x2c, 0x35, "Y" },
	{ 0x2d, 0x3c, "U" }, { 0x2e, 0x43, "I" }, { 0x2f, 0x44, "O" },
	{ 0x30, 0x4d, "P" }, { 0x31, 0x54, "LeftBracket" },
	{ 0x32, 0x5b, "RightBracket" }, { 0x33, 0x5d, "Backslash" },
	{ 0x34, 0xe071, "Delete" }, { 0x35, 0xe069, "End" },
	{ 0x36, 0xe07a, "PageDown" },
	{ 0x37, 0x6c, "Keypad7" }, { 0x38, 0x75, "Keypad8" },
	{ 0x39, 0x7d, "Keypad9" }, { 0x3a, 0x79, "KeypadPlus" },
	{ 0x3b, 0x58, "CapsLock" },
	{ 0x3c, 0x1c, "A" }, { 0x3d, 0x1b, "S" }, { 0x3e, 0x23, "D" },
	{ 0x3f, 0x2b, "F" }, { 0x40, 0x34, "G" }, { 0x41, 0x33, "H" },
	{ 0x42, 0x3b, "J" }, { 0x43, 0x42, "K" }, { 0x44, 0x4b, "L" },
	{ 0x45, 0x4c, "Semicolon" }, { 0x46, 0x52, "Quote" },
	{ 0x47, 0x5a, "Enter" },
	{ 0x48, 0x6b, "Keypad4" }, { 0x49, 0x73, "Keypad5" },
	{ 0x4a, 0x74, "Keypad6" },
	{ 0x4b, 0x12, "LeftShift" },
	{ 0x4c, 0x1a, "Z" }, { 0x4d, 0x22, "X" }, { 0x4e, 0x21, "C" },
	{ 0x4f, 0x2a, "V" }, { 0x50, 0x32, "B" }, { 0x51, 0x31, "N" },
	{ 0x52, 0x3a, "M" }, { 0x53, 0x41, "Comma" }, { 0x54, 0x49, "Period" },
	{ 0x55, 0x4a, "Slash" }, { 0x56, 0x59, "RightShift" },
	{ 0x57, 0xe075, "Up" },
	{ 0x58, 0x69, "Keypad1" }, { 0x59, 0x72, "Keypad2" },
	{ 0x5a, 0x7a, "Keypad3" }, { 0x5b, 0xe05a, "KeypadEnter" },
	{ 0x5c, 0x14, "LeftControl" }, { 0x5d, 0x11, "LeftAlt" },
	{ 0x5e, 0x29, "Space" }, { 0x5f, 0xe011, "RightAlt" },
	{ 0x60, 0xe014, "RightControl" },
	{ 0x61, 0xe06b, "Left" }, { 0x62, 0xe072, "Down" },
	{ 0x63, 0xe074, "Right" },
	{ 0x64, 0x70, "Keypad0" }, { 0x65, 0x71, "KeypadPeriod" },
	{ 0x66, 0xe01f, "LeftWindows" }, { 0x67, 0xe027, "RightWindows" },
	{ 0x68, 0xe02f, "Menu" }, { 0x69, 0x61, "LessGreater" },
	{ 0x7e, 0xe07c, "SysRq" }, { 0x7f, 0xe11477, "Break" },
};

}	// namespace


u32
HaikuKeyToSet2(u32 haikuKey)
{
	for (const Key& key : kKeys) {
		if (key.haiku == haikuKey)
			return key.set2;
	}
	return 0;
}


u32
KeyNameToSet2(const char* name)
{
	for (const Key& key : kKeys) {
		if (strcasecmp(key.name, name) == 0)
			return key.set2;
	}
	return 0;
}


u32
AsciiToSet2(char c, bool& shifted)
{
	static const char* const kShifted = "~!@#$%^&*()_+{}|:\"<>?";
	static const char* const kUnshifted = "`1234567890-=[]\\;',./";
	static const char* const kNames[] = { "Grave", "1", "2", "3", "4", "5",
		"6", "7", "8", "9", "0", "Minus", "Equal", "LeftBracket",
		"RightBracket", "Backslash", "Semicolon", "Quote", "Comma", "Period",
		"Slash" };
	shifted = false;
	char name[2] = { 0, 0 };
	if (c >= 'a' && c <= 'z') {
		name[0] = (char)(c - 32);
		return KeyNameToSet2(name);
	}
	if (c >= 'A' && c <= 'Z') {
		name[0] = c;
		shifted = true;
		return KeyNameToSet2(name);
	}
	switch (c) {
		case ' ': return KeyNameToSet2("Space");
		case '\n': return KeyNameToSet2("Enter");
		case '\t': return KeyNameToSet2("Tab");
		case 0: return 0;
	}
	if (const char* p = strchr(kShifted, c)) {
		shifted = true;
		return KeyNameToSet2(kNames[p - kShifted]);
	}
	if (const char* p = strchr(kUnshifted, c))
		return KeyNameToSet2(kNames[p - kUnshifted]);
	return 0;
}

}	// namespace bebox
