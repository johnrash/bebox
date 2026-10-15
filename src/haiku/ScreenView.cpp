/*
 * BeBox emulator for Haiku - the emulated display.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "ScreenView.h"

#include "BeBox.h"
#include "core/board/Machine.h"
#include "core/host/EmulatorThread.h"
#include "core/isa/Kbc8042.h"

#include <Application.h>
#include <Bitmap.h>
#include <Cursor.h>
#include <Messenger.h>
#include <Region.h>
#include <Screen.h>
#include <Window.h>
#include <WindowScreen.h>

#include <algorithm>
#include <cmath>


ScreenView::ScreenView(bebox::EmulatorThread& emulator)
	:
	BView("screen", B_WILL_DRAW | B_FRAME_EVENTS | B_NAVIGABLE),
	fEmulator(emulator),
	fStagingWidth(640),
	fStagingHeight(480),
	fFramePending(false),
	fBitmap(nullptr),
	fScale(1),
	fCaptured(false),
	fPowered(false),
	fButtons(0),
	fHaveLastWhere(false),
	fMouseSpeed(100),
	fSwapButtons(false),
	fCaptureOnClick(true),
	fRemainderX(0),
	fRemainderY(0)
{
	SetViewColor(B_TRANSPARENT_COLOR);
	fStaging.assign(640 * 480, 0);
}


ScreenView::~ScreenView()
{
	SetMouseCaptured(false);
	delete fBitmap;
}


void
ScreenView::AttachedToWindow()
{
	BView::AttachedToWindow();
	MakeFocus(true);
}


void
ScreenView::SetScale(int32 scale)
{
	fScale = scale;
	InvalidateLayout();
	Invalidate();
}


BSize
ScreenView::FrameSize() const
{
	return BSize(fStagingWidth, fStagingHeight);
}


BSize
ScreenView::MinSize()
{
	if (fScale == 0)
		return BSize(160, 120);
	return BSize(fStagingWidth * fScale - 1, fStagingHeight * fScale - 1);
}


BSize
ScreenView::PreferredSize()
{
	int scale = fScale == 0 ? 1 : fScale;
	return BSize(fStagingWidth * scale - 1, fStagingHeight * scale - 1);
}


void
ScreenView::FrameReady(const uint32* pixels, int width, int height,
	int stride)
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		fStaging.resize((size_t)width * height);
		for (int y = 0; y < height; y++) {
			memcpy(&fStaging[(size_t)y * width], pixels + (size_t)y * stride,
				width * 4);
		}
		fStagingWidth = width;
		fStagingHeight = height;
	}
	if (!fFramePending.exchange(true))
		BMessenger(this).SendMessage(kMsgFrame);
}


void
ScreenView::_UpdateBitmap()
{
	std::lock_guard<std::mutex> lock(fLock);
	BRect frame(0, 0, fStagingWidth - 1, fStagingHeight - 1);
	bool resized = fBitmap == nullptr || fBitmap->Bounds() != frame;
	if (resized) {
		delete fBitmap;
		fBitmap = new BBitmap(frame, B_RGB32);
	}
	uint8* bits = (uint8*)fBitmap->Bits();
	int32 rowBytes = fBitmap->BytesPerRow();
	for (int y = 0; y < fStagingHeight; y++) {
		memcpy(bits + (size_t)y * rowBytes,
			&fStaging[(size_t)y * fStagingWidth], fStagingWidth * 4);
	}
	if (resized && Window() != nullptr) {
		InvalidateLayout();
		BMessage message(kMsgFrame);
		message.AddBool("resized", true);
		Window()->PostMessage(&message);
	}
}


BBitmap*
ScreenView::CopyFrame() const
{
	if (fBitmap == nullptr)
		return nullptr;
	return new BBitmap(fBitmap);
}


BRect
ScreenView::_DestinationRect() const
{
	BRect bounds = Bounds();
	float width = fStagingWidth;
	float height = fStagingHeight;
	float scale;
	if (fScale == 0) {
		scale = std::min((bounds.Width() + 1) / width,
			(bounds.Height() + 1) / height);
	} else
		scale = fScale;
	float w = width * scale;
	float h = height * scale;
	float left = bounds.left + floorf((bounds.Width() + 1 - w) / 2);
	float top = bounds.top + floorf((bounds.Height() + 1 - h) / 2);
	return BRect(left, top, left + w - 1, top + h - 1);
}


void
ScreenView::Draw(BRect updateRect)
{
	BRect destination = _DestinationRect();
	SetHighColor(0, 0, 0);
	BRegion background(Bounds());
	background.Exclude(destination);
	FillRegion(&background);
	if (fBitmap != nullptr) {
		uint32 options = fScale == 0 && destination.Width() + 1
			!= fStagingWidth ? B_FILTER_BITMAP_BILINEAR : 0;
		DrawBitmap(fBitmap, fBitmap->Bounds(), destination, options);
	} else {
		FillRect(destination);
	}
}


void
ScreenView::FrameResized(float width, float height)
{
	Invalidate();
}


void
ScreenView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgFrame:
			fFramePending = false;
			_UpdateBitmap();
			Invalidate(_DestinationRect());
			break;
		default:
			BView::MessageReceived(message);
			break;
	}
}


BPoint
ScreenView::_ScreenCenter() const
{
	BRect frame = ConvertToScreen(Bounds());
	return BPoint(floorf((frame.left + frame.right) / 2),
		floorf((frame.top + frame.bottom) / 2));
}


void
ScreenView::SetPowered(bool powered)
{
	fPowered = powered;
	if (powered)
		return;
	SetMouseCaptured(false);
	{
		// the emulation thread has stopped: nothing refills the frame
		std::lock_guard<std::mutex> lock(fLock);
		std::fill(fStaging.begin(), fStaging.end(), 0);
	}
	_UpdateBitmap();
	Invalidate();
}


void
ScreenView::SetMouseCaptured(bool captured)
{
	if (captured && !fPowered)
		return;
	if (captured == fCaptured)
		return;
	fCaptured = captured;
	if (captured) {
		be_app->HideCursor();
		SetEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS
			| B_NO_POINTER_HISTORY);
		BPoint center = _ScreenCenter();
		fHaveLastWhere = false;
		set_mouse_position((int32)center.x, (int32)center.y);
	} else {
		SetEventMask(0, 0);
		be_app->ShowCursor();
		fButtons = 0;
		_SendMouse(0, 0, 0);
	}
	if (Window() != nullptr)
		Window()->PostMessage(kMsgCaptureChanged);
}


void
ScreenView::SetMouseOptions(int32 speed, bool swapButtons,
	bool captureOnClick)
{
	fMouseSpeed = std::max<int32>(10, std::min<int32>(1000, speed));
	fSwapButtons = swapButtons;
	fCaptureOnClick = captureOnClick;
}


uint8
ScreenView::_Buttons() const
{
	uint8 buttons = 0;
	uint8 primary = fSwapButtons ? 2 : 1;
	uint8 secondary = fSwapButtons ? 1 : 2;
	if ((fButtons & B_PRIMARY_MOUSE_BUTTON) != 0)
		buttons |= primary;
	if ((fButtons & B_SECONDARY_MOUSE_BUTTON) != 0)
		buttons |= secondary;
	if ((fButtons & B_TERTIARY_MOUSE_BUTTON) != 0)
		buttons |= 4;
	return buttons;
}


void
ScreenView::_SendMouse(int dx, int dy, int dz)
{
	uint8 buttons = _Buttons();
	fEmulator.Post([dx, dy, dz, buttons](bebox::Machine& machine) {
		machine.Keyboard().MouseMoved(dx, dy, dz, buttons);
	});
}


void
ScreenView::MouseDown(BPoint where)
{
	MakeFocus(true);
	int32 buttons = 0;
	Window()->CurrentMessage()->FindInt32("buttons", &buttons);
	if (!fCaptured) {
		if (fCaptureOnClick && (buttons & B_PRIMARY_MOUSE_BUTTON) != 0)
			SetMouseCaptured(true);
		return;
	}
	fButtons = buttons;
	_SendMouse(0, 0, 0);
}


void
ScreenView::MouseUp(BPoint where)
{
	if (!fCaptured)
		return;
	int32 buttons = 0;
	Window()->CurrentMessage()->FindInt32("buttons", &buttons);
	fButtons = buttons;
	_SendMouse(0, 0, 0);
}


// Motion of the captured mouse. A mouse or trackpad reports its own motion
// ("be:delta_x/y", added by the input_server): the pointer goes back to the
// middle of the screen now and then so that it never sticks at an edge.
// Absolute pointing devices (tablets, the pointer of a VNC viewer) only
// give positions: their motion is the change of position, and they are
// never moved (the next position would come from the device again, far
// from where the pointer was put). The event that putting the pointer
// somewhere makes ("be:set_mouse") is no motion, and an absolute device's
// next position starts afresh after it.
void
ScreenView::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	if (!fCaptured)
		return;
	BPoint screenWhere = ConvertToScreen(where);
	BMessage* message = Window()->CurrentMessage();
	int32 deltaX, deltaY;
	float moveX, moveY;		// screen direction: positive y is down
	bool relative = message != nullptr
		&& message->FindInt32("be:delta_x", &deltaX) == B_OK
		&& message->FindInt32("be:delta_y", &deltaY) == B_OK;
	if (message != nullptr && message->GetBool("be:set_mouse", false)) {
		fHaveLastWhere = false;
		return;
	}
	if (relative) {
		moveX = deltaX;
		moveY = -deltaY;
	} else {
		if (!fHaveLastWhere) {
			fLastWhere = screenWhere;
			fHaveLastWhere = true;
			return;
		}
		moveX = screenWhere.x - fLastWhere.x;
		moveY = screenWhere.y - fLastWhere.y;
	}
	fLastWhere = screenWhere;
	fHaveLastWhere = true;

	moveX = moveX * fMouseSpeed / 100.0f + fRemainderX;
	moveY = moveY * fMouseSpeed / 100.0f + fRemainderY;
	int dx = (int)moveX;
	int dy = (int)moveY;
	fRemainderX = moveX - dx;
	fRemainderY = moveY - dy;
	if (dx != 0 || dy != 0) {
		int32 buttons = 0;
		if (message != nullptr)
			message->FindInt32("buttons", &buttons);
		fButtons = buttons;
		// PS/2: positive y is up.
		_SendMouse(dx, -dy, 0);
	}

	if (relative) {
		BPoint center = _ScreenCenter();
		if (fabsf(screenWhere.x - center.x) > 64
			|| fabsf(screenWhere.y - center.y) > 64)
			set_mouse_position((int32)center.x, (int32)center.y);
	}
}
