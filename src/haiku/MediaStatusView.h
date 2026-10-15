/*
 * BeBox emulator for Haiku - media icons in the status line.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Haiku's floppy, CD and hard disk icons: faded when the drive is empty,
 * the hard disk with an amber dot while the disk LED is lit. Clicking the
 * floppy inserts a disk.
 */
#ifndef BEBOX_MEDIA_STATUS_VIEW_H
#define BEBOX_MEDIA_STATUS_VIEW_H

#include <String.h>
#include <View.h>

class BBitmap;

class MediaStatusView : public BView {
public:
	enum {
		kFloppy = 0,
		kCd,
		kDisk,
		kCount
	};

								MediaStatusView();
	virtual						~MediaStatusView();

			// 'descriptions' are the tool tip lines (image names).
			void				SetState(const bool present[kCount],
									bool diskActive,
									const BString& description);

	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	BSize				MinSize();
	virtual	BSize				MaxSize();
	virtual	BSize				PreferredSize();

private:
			BRect				_IconFrame(int index) const;

			BBitmap*			fIcons[kCount];
			bool				fPresent[kCount];
			bool				fDiskActive;
			float				fIconSize;
			BString				fDescription;
};

#endif	// BEBOX_MEDIA_STATUS_VIEW_H
