/*
 * BeBox emulator for Haiku - serial console windows.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "SerialConsole.h"

#include "BeBox.h"

#include <LayoutBuilder.h>
#include <ScrollView.h>
#include <TextView.h>

static const size_t kMaxHistory = 256 * 1024;

enum {
	kMsgSerialOutput = 'sout'
};


ConsoleSerial::ConsoleSerial(int port)
	:
	fPort(port),
	fDropped(0),
	fNotified(false)
{
}


void
ConsoleSerial::SetWindow(BMessenger window)
{
	std::lock_guard<std::mutex> lock(fLock);
	fWindow = window;
	fNotified = false;
}


size_t
ConsoleSerial::OutputSince(size_t offset, std::string& text)
{
	std::lock_guard<std::mutex> lock(fLock);
	fNotified = false;
	if (offset < fDropped)
		offset = fDropped;
	text = fOutput.substr(offset - fDropped);
	return fDropped + fOutput.size();
}


void
ConsoleSerial::Type(const char* bytes, size_t length)
{
	std::lock_guard<std::mutex> lock(fLock);
	for (size_t i = 0; i < length; i++)
		fInput.push_back((bebox::u8)bytes[i]);
}


void
ConsoleSerial::Transmit(bebox::u8 byte)
{
	BMessenger notify;
	{
		std::lock_guard<std::mutex> lock(fLock);
		fOutput.push_back((char)byte);
		if (fOutput.size() > kMaxHistory) {
			size_t trim = fOutput.size() - kMaxHistory / 2;
			fOutput.erase(0, trim);
			fDropped += trim;
		}
		// one notification until the window has fetched the text
		if (!fNotified && fWindow.IsValid()) {
			fNotified = true;
			notify = fWindow;
		}
	}
	if (notify.IsValid()) {
		// never block the emulation on a busy window
		BMessage message(kMsgSerialOutput);
		if (notify.SendMessage(&message, (BHandler*)nullptr, 0) != B_OK) {
			std::lock_guard<std::mutex> lock(fLock);
			fNotified = false;
		}
	}
}


int
ConsoleSerial::Receive()
{
	std::lock_guard<std::mutex> lock(fLock);
	if (fInput.empty())
		return -1;
	bebox::u8 byte = fInput.front();
	fInput.pop_front();
	return byte;
}


// #pragma mark - window


// The text view: typing goes to the serial port, not into the text.
class ConsoleTextView : public BTextView {
public:
	ConsoleTextView(ConsoleSerial** serial)
		:
		BTextView("console"),
		fSerial(serial)
	{
		MakeEditable(false);
		SetStylable(false);
		SetWordWrap(true);
		BFont font(be_fixed_font);
		SetFontAndColor(&font);
	}

	virtual void KeyDown(const char* bytes, int32 count)
	{
		if (*fSerial == nullptr)
			return;
		if (count == 1 && bytes[0] == B_ENTER)
			(*fSerial)->Type("\r", 1);
		else if (count == 1 && bytes[0] == B_BACKSPACE)
			(*fSerial)->Type("\b", 1);
		else
			(*fSerial)->Type(bytes, count);
	}

private:
	ConsoleSerial** fSerial;
};


SerialConsoleWindow::SerialConsoleWindow(ConsoleSerial* serial)
	:
	BWindow(BRect(120, 120, 760, 520), "Serial console", B_DOCUMENT_WINDOW,
		B_AUTO_UPDATE_SIZE_LIMITS),
	fSerial(nullptr),
	fOffset(0),
	fCarriageReturn(false),
	fLastBreak(0)
{
	ConsoleTextView* text = new ConsoleTextView(&fSerial);
	fText = text;
	BScrollView* scroll = new BScrollView("scroll", fText, 0, false, true,
		B_NO_BORDER);
	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(scroll);
	SetSerial(serial);
}


void
SerialConsoleWindow::SetSerial(ConsoleSerial* serial)
{
	if (fSerial != nullptr)
		fSerial->SetWindow(BMessenger());
	fSerial = serial;
	if (fSerial != nullptr) {
		BString title;
		title.SetToFormat("Serial %d", fSerial->Port() + 1);
		SetTitle(title.String());
		fSerial->SetWindow(BMessenger(this));
	}
	_Update();
}


void
SerialConsoleWindow::_Update()
{
	if (fSerial == nullptr)
		return;
	std::string text;
	fOffset = fSerial->OutputSince(fOffset, text);
	if (text.empty())
		return;
	// CR LF (and lone CR) become line breaks; other control
	// characters except tabs are left out
	BString clean;
	for (char c : text) {
		// CR LF, LF CR, a lone CR or LF: one line break
		if (c == '\r' || c == '\n') {
			char other = c == '\r' ? '\n' : '\r';
			if (fCarriageReturn && fLastBreak == other) {
				fCarriageReturn = false;
				continue;
			}
			clean << '\n';
			fCarriageReturn = true;
			fLastBreak = c;
			continue;
		}
		fCarriageReturn = false;
		if (c == '\b') {
			if (clean.Length() > 0)
				clean.Truncate(clean.Length() - 1);
			else if (fText->TextLength() > 0)
				fText->Delete(fText->TextLength() - 1, fText->TextLength());
			continue;
		}
		if ((unsigned char)c < 0x20 && c != '\n' && c != '\t')
			continue;
		clean << c;
	}
	fText->Insert(fText->TextLength(), clean.String(), clean.Length());
	// keep the view from growing without limit
	if (fText->TextLength() > (int32)kMaxHistory)
		fText->Delete(0, fText->TextLength() - (int32)kMaxHistory / 2);
	fText->ScrollToOffset(fText->TextLength());
}


void
SerialConsoleWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgSerialOutput:
			_Update();
			break;
		default:
			BWindow::MessageReceived(message);
			break;
	}
}


bool
SerialConsoleWindow::QuitRequested()
{
	// the backend keeps the history for the next time
	if (fSerial != nullptr)
		fSerial->SetWindow(BMessenger());
	return true;
}
