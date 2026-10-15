/*
 * BeBox emulator for Haiku - one column of the front panel LEDs.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Sixteen LEDs behind one long clear light pipe: fifteen load LEDs (the
 * bar graph level 0-15 of one processor) above the power LED (left column)
 * or the disk activity LED (right column). The pipe diffuses their light:
 * each LED is a soft patch that blends into the next, with a darker
 * gradient between two lit ones.
 */
#ifndef BEBOX_LED_COLUMN_VIEW_H
#define BEBOX_LED_COLUMN_VIEW_H

#include <View.h>

class BBitmap;

class LedColumnView : public BView {
public:
								LedColumnView(const char* name, bool withDisk);
	virtual						~LedColumnView();

			void				SetLevels(float level, bool power, float disk);
			void				SetStyle(int32 style);

	virtual	void				Draw(BRect updateRect);
	virtual	BSize				MinSize();
	virtual	BSize				MaxSize();
	virtual	BSize				PreferredSize();

private:
			void				_RenderPipe(int32 rows);

			bool				fWithDisk;
			float				fLevel;
			bool				fPower;
			float				fDisk;
			int32				fStyle;
			BBitmap*			fPipe;		// one pixel wide, a row per line
};

#endif	// BEBOX_LED_COLUMN_VIEW_H
