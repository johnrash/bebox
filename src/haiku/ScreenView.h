/*
 * BeBox emulator for Haiku - the emulated display.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Frames arrive on the emulation thread (VideoSink); they are copied to a
 * staging buffer and the view is told to redraw from its window thread.
 * A click captures the mouse (relative PS/2 motion); the host key
 * releases it.
 */
#ifndef BEBOX_SCREEN_VIEW_H
#define BEBOX_SCREEN_VIEW_H

#include "core/host/VideoSink.h"

#include <View.h>

#include <atomic>
#include <mutex>
#include <vector>

class BBitmap;

namespace bebox {
class EmulatorThread;
}

class ScreenView : public BView, public bebox::VideoSink {
public:
								ScreenView(bebox::EmulatorThread& emulator);
	virtual						~ScreenView();

			// 1, 2 or 0 (fit the view)
			void				SetScale(int32 scale);
			int32				Scale() const { return fScale; }
			BSize				FrameSize() const;
			bool				IsMouseCaptured() const { return fCaptured; }
			void				SetMouseCaptured(bool captured);
			// Powered off: a black screen that does not capture the mouse.
			void				SetPowered(bool powered);
			// Motion scaling (percent), swapped primary and secondary
			// buttons, capture by clicking the screen.
			void				SetMouseOptions(int32 speed, bool swapButtons,
									bool captureOnClick);
			BBitmap*			CopyFrame() const;

	// VideoSink (emulation thread)
	virtual	void				FrameReady(const uint32* pixels, int width,
									int height, int stride);

	// BView
	virtual	void				AttachedToWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseUp(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				FrameResized(float width, float height);
	virtual	BSize				MinSize();
	virtual	BSize				PreferredSize();

private:
			void				_UpdateBitmap();
			BRect				_DestinationRect() const;
			void				_SendMouse(int dx, int dy, int dz);
			uint8				_Buttons() const;
			BPoint				_ScreenCenter() const;

			bebox::EmulatorThread& fEmulator;
			std::mutex			fLock;
			std::vector<uint32>	fStaging;
			int					fStagingWidth;
			int					fStagingHeight;
			std::atomic<bool>	fFramePending;
			BBitmap*			fBitmap;
			int32				fScale;
			bool				fCaptured;
			bool				fPowered;
			uint32				fButtons;
			// captured mouse: the last position of an absolute device
			BPoint				fLastWhere;
			bool				fHaveLastWhere;
			int32				fMouseSpeed;
			bool				fSwapButtons;
			bool				fCaptureOnClick;
			float				fRemainderX;	// fractions of scaled motion
			float				fRemainderY;
};

#endif	// BEBOX_SCREEN_VIEW_H
