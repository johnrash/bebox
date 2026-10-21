/*
 * BeBox emulator for Haiku - media icons in the status line.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "MediaStatusView.h"

#include "BeBox.h"

#include <Application.h>
#include <Bitmap.h>
#include <ControlLook.h>
#include <IconUtils.h>
#include <Resources.h>
#include <Window.h>

#include <algorithm>

static const char* const kIconNames[MediaStatusView::kCount] = { "floppy",
	"cd", "harddisk" };


MediaStatusView::MediaStatusView()
	:
	BView("media", B_WILL_DRAW),
	fDiskActive(false)
{
	fIconSize = be_control_look->ComposeIconSize(16).Height() + 1;
	for (int i = 0; i < kCount; i++) {
		fPresent[i] = false;
		fIcons[i] = nullptr;
		size_t length = 0;
		const void* data = BApplication::AppResources()->LoadResource('VICN',
			kIconNames[i], &length);
		if (data == nullptr)
			continue;
		fIcons[i] = new BBitmap(BRect(0, 0, fIconSize - 1, fIconSize - 1),
			B_RGBA32);
		if (BIconUtils::GetVectorIcon((const uint8*)data, length, fIcons[i])
				!= B_OK) {
			delete fIcons[i];
			fIcons[i] = nullptr;
		}
	}
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
}


MediaStatusView::~MediaStatusView()
{
	for (int i = 0; i < kCount; i++)
		delete fIcons[i];
}


void
MediaStatusView::SetState(const bool present[kCount], bool diskActive,
	const BString& description)
{
	bool changed = diskActive != fDiskActive;
	for (int i = 0; i < kCount; i++) {
		changed |= present[i] != fPresent[i];
		fPresent[i] = present[i];
	}
	fDiskActive = diskActive;
	if (description != fDescription) {
		fDescription = description;
		SetToolTip(fDescription.String());
	}
	if (changed)
		Invalidate();
}


BRect
MediaStatusView::_IconFrame(int index) const
{
	float spacing = be_control_look->DefaultLabelSpacing();
	float left = index * (fIconSize + spacing);
	float top = (Bounds().Height() - fIconSize) / 2;
	return BRect(left, top, left + fIconSize - 1, top + fIconSize - 1);
}


void
MediaStatusView::Draw(BRect updateRect)
{
	SetDrawingMode(B_OP_ALPHA);
	for (int i = 0; i < kCount; i++) {
		if (fIcons[i] == nullptr)
			continue;
		BRect frame = _IconFrame(i);
		if (fPresent[i]) {
			SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
			DrawBitmap(fIcons[i], frame.LeftTop());
		} else {
			// faded: an empty drive
			SetHighColor(0, 0, 0, 70);
			SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_COMPOSITE);
			DrawBitmap(fIcons[i], frame.LeftTop());
		}
		if (i == kDisk && fDiskActive) {
			SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
			SetHighColor(255, 176, 0, 255);
			float dot = std::max(3.0f, fIconSize / 4);
			FillEllipse(BRect(frame.right - dot + 1, frame.bottom - dot + 1,
				frame.right, frame.bottom));
		}
	}
	SetDrawingMode(B_OP_COPY);
}


void
MediaStatusView::MouseDown(BPoint where)
{
	if (Window() == nullptr)
		return;
	if (_IconFrame(kFloppy).Contains(where))
		Window()->PostMessage(kMsgInsertFloppy);
	else if (_IconFrame(kCd).Contains(where))
		Window()->PostMessage(kMsgInsertCd);
}


BSize
MediaStatusView::MinSize()
{
	float spacing = be_control_look->DefaultLabelSpacing();
	return BSize(kCount * fIconSize + (kCount - 1) * spacing, fIconSize);
}


BSize
MediaStatusView::MaxSize()
{
	return MinSize();
}


BSize
MediaStatusView::PreferredSize()
{
	return MinSize();
}
