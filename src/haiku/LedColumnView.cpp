/*
 * BeBox emulator for Haiku - one column of the front panel LEDs.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "LedColumnView.h"

#include "BeBox.h"

#include <Bitmap.h>

#include <algorithm>
#include <cmath>

static const int kLoadLeds = 15;
static const int kSlots = kLoadLeds + 1;
static const float kWidth = 22;

// How an LED's light spreads along the pipe, by the distance from its
// centre in LED spacings: a flat topped core plus a faint wider glow,
// together down to a third halfway to the next LED (so two lit LEDs show
// a darker band between them, and an unlit one catches some light).
static const float kCoreWidth = 0.46f;
static const float kCoreSharpness = 3;
static const float kGlow = 0.15f;
static const float kGlowWidth = 0.7f;
// An unlit LED still shows faintly through the pipe.
static const float kUnlit = 0.10f;
// The pipe itself, dark smoky acrylic.
static const rgb_color kAcrylic = { 10, 12, 10, 255 };


static float
LightSpread(float distance)
{
	distance = std::fabs(distance);
	float core = std::exp(-std::pow(distance / kCoreWidth, kCoreSharpness));
	float glow = std::exp(-(distance / kGlowWidth) * (distance / kGlowWidth));
	return (1 - kGlow) * core + kGlow * glow;
}


LedColumnView::LedColumnView(const char* name, bool withDisk)
	:
	BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
	fWithDisk(withDisk),
	fLevel(0),
	fPower(false),
	fDisk(0),
	fStyle(kLedStyleAuthentic),
	fPipe(nullptr)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


LedColumnView::~LedColumnView()
{
	delete fPipe;
}


void
LedColumnView::SetLevels(float level, bool power, float disk)
{
	// afterglow: a disk access shows for a moment, like the real LED seen
	// through the light pipe
	disk = std::max(disk, fDisk * 0.45f);
	if (disk < 0.03f)
		disk = 0;
	if (std::fabs(level - fLevel) < 0.05f && power == fPower
		&& std::fabs(disk - fDisk) < 0.03f)
		return;
	fLevel = level;
	fPower = power;
	fDisk = disk;
	Invalidate();
}


void
LedColumnView::SetStyle(int32 style)
{
	fStyle = style;
	Invalidate();
}


BSize
LedColumnView::MinSize()
{
	return BSize(kWidth, 120);
}


BSize
LedColumnView::MaxSize()
{
	return BSize(kWidth, B_SIZE_UNLIMITED);
}


BSize
LedColumnView::PreferredSize()
{
	return BSize(kWidth, 480);
}


// The light along the pipe, from the top row down: the LEDs' light adds up.
void
LedColumnView::_RenderPipe(int32 rows)
{
	rgb_color lime = { 150, 255, 40, 255 };
	rgb_color green = { 40, 230, 60, 255 };
	rgb_color amber = { 255, 170, 0, 255 };
	rgb_color red = { 255, 40, 30, 255 };
	// the power LED matches the load LEDs
	rgb_color load = fStyle == kLedStyleRedGreen ? green : lime;

	// From the bottom: the power LED (left column) or the disk LED (right
	// column), then the fifteen load LEDs.
	rgb_color colors[kSlots];
	float brightness[kSlots];
	for (int i = 0; i < kSlots; i++) {
		if (i == 0) {
			if (fWithDisk) {
				colors[i] = fStyle == kLedStyleRedGreen ? red : amber;
				brightness[i] = fDisk;
			} else {
				colors[i] = load;
				brightness[i] = fPower ? 1 : 0;
			}
		} else {
			int led = i - 1;
			colors[i] = fStyle == kLedStyleRedGreen && led >= kLoadLeds - 3
				? red : load;
			brightness[i] = fPower ? fLevel - led : 0;
		}
		brightness[i] = std::max(0.0f, std::min(1.0f, brightness[i]));
	}

	float slot = rows / (float)kSlots;
	uint8* bits = (uint8*)fPipe->Bits();
	int32 bytesPerRow = fPipe->BytesPerRow();
	for (int32 y = 0; y < rows; y++) {
		// in LED spacings from the bottom of the pipe
		float position = (rows - y - 0.5f) / slot;
		float red = kAcrylic.red;
		float green = kAcrylic.green;
		float blue = kAcrylic.blue;
		for (int i = 0; i < kSlots; i++) {
			float distance = position - (i + 0.5f);
			if (std::fabs(distance) > 2.5f)
				continue;
			float light = LightSpread(distance)
				* (kUnlit + (1 - kUnlit) * brightness[i]);
			red += colors[i].red * light;
			green += colors[i].green * light;
			blue += colors[i].blue * light;
		}
		uint8* pixel = bits + y * bytesPerRow;
		pixel[0] = (uint8)std::min(255.0f, blue);
		pixel[1] = (uint8)std::min(255.0f, green);
		pixel[2] = (uint8)std::min(255.0f, red);
		pixel[3] = 255;
	}
}


void
LedColumnView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	// the black frame around the light pipe
	SetHighColor(12, 12, 14);
	FillRect(bounds);

	BRect pipe(bounds.left + 4, bounds.top + 6, bounds.right - 4,
		bounds.bottom - 6);
	int32 rows = pipe.IntegerHeight() + 1;
	if (rows < kSlots || !pipe.IsValid())
		return;
	if (fPipe == nullptr || fPipe->Bounds().IntegerHeight() + 1 != rows) {
		delete fPipe;
		fPipe = new BBitmap(BRect(0, 0, 0, rows - 1), B_RGB32);
	}
	_RenderPipe(rows);
	DrawBitmap(fPipe, fPipe->Bounds(), pipe);
}
