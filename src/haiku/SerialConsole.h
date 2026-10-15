/*
 * BeBox emulator for Haiku - serial console windows.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * ConsoleSerial is the backend of a serial port set to "Console window":
 * it keeps what the guest sent (so a console opened later shows the
 * history) and queues what is typed in the window. Both sides lock.
 */
#ifndef BEBOX_SERIAL_CONSOLE_H
#define BEBOX_SERIAL_CONSOLE_H

#include "core/host/SerialBackend.h"

#include <Messenger.h>
#include <Window.h>

#include <deque>
#include <mutex>
#include <string>

class BTextView;

class ConsoleSerial : public bebox::SerialBackend {
public:
								ConsoleSerial(int port);

			int					Port() const { return fPort; }
			void				SetWindow(BMessenger window);
			// Output since 'offset'; returns the new offset.
			size_t				OutputSince(size_t offset, std::string& text);
			void				Type(const char* bytes, size_t length);

	// SerialBackend (emulation thread)
	virtual	void				Transmit(bebox::u8 byte);
	virtual	int					Receive();

private:
			int					fPort;
			std::mutex			fLock;
			std::string			fOutput;
			size_t				fDropped;		// bytes trimmed from fOutput
			std::deque<bebox::u8> fInput;
			BMessenger			fWindow;
			bool				fNotified;
};


class SerialConsoleWindow : public BWindow {
public:
								SerialConsoleWindow(ConsoleSerial* serial);

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

			void				SetSerial(ConsoleSerial* serial);

private:
			void				_Update();

			ConsoleSerial*		fSerial;
			BTextView*			fText;
			size_t				fOffset;
			bool				fCarriageReturn;	// just broke a line
			char				fLastBreak;
};

#endif	// BEBOX_SERIAL_CONSOLE_H
