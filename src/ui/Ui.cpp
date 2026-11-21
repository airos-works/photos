#include "Ui.h"

#include "core/Util.h"

#include <Shape.h>

#include <cmath>
#include <cstdio>

namespace ui {

float
Scale()
{
	static float scale = 0;
	if (scale == 0) {
		scale = be_plain_font->Size() / 12.0f;
		if (scale < 0.75f)
			scale = 0.75f;
	}
	return scale;
}


BString
MonthName(int month)
{
	static const char* const kNames[] = {"January", "February", "March", "April",
		"May", "June", "July", "August", "September", "October", "November",
		"December"};
	return month >= 1 && month <= 12 ? kNames[month - 1] : "";
}


BString
MonthTitle(int64_t taken)
{
	photos::CivilTime time = photos::SecondsToCivil(taken);
	BString text;
	text.SetToFormat("%s %d", MonthName(time.month).String(), time.year);
	return text;
}


BString
DayTitle(int64_t taken)
{
	static const char* const kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
		"Thursday", "Friday", "Saturday"};
	photos::CivilTime time = photos::SecondsToCivil(taken);
	BString text;
	text.SetToFormat("%s, %d %s %d", kDays[photos::DayOfWeek(taken)], time.day,
		MonthName(time.month).String(), time.year);
	return text;
}


BString
DateTimeText(int64_t taken)
{
	if (taken == 0)
		return "No date";
	photos::CivilTime time = photos::SecondsToCivil(taken);
	BString text;
	text.SetToFormat("%d %s %d, %d:%02d", time.day, MonthName(time.month).String(),
		time.year, time.hour, time.minute);
	return text;
}


BString
Number(int value)
{
	char digits[32];
	snprintf(digits, sizeof(digits), "%d", value < 0 ? -value : value);
	BString text;
	int length = (int)strlen(digits);
	for (int i = 0; i < length; i++) {
		if (i > 0 && (length - i) % 3 == 0)
			text << ",";
		text << digits[i];
	}
	if (value < 0)
		text.Prepend("-");
	return text;
}


BString
CountText(int count, const char* one, const char* many)
{
	BString text = Number(count);
	text << " " << (count == 1 ? one : many);
	return text;
}


void
DrawTruncated(BView* view, const char* text, BPoint where, float width)
{
	BString string(text);
	BFont font;
	view->GetFont(&font);
	if (font.StringWidth(string.String()) > width)
		font.TruncateString(&string, B_TRUNCATE_END, width);
	view->DrawString(string.String(), where);
}


void
FillRound(BView* view, BRect rect, float radius)
{
	view->FillRoundRect(rect, radius, radius);
}


void
StrokeRound(BView* view, BRect rect, float radius)
{
	view->StrokeRoundRect(rect, radius, radius);
}


void
DrawHeart(BView* view, BRect rect, bool filled)
{
	float w = rect.Width(), h = rect.Height();
	float x = rect.left, y = rect.top;
	BShape shape;
	shape.MoveTo(BPoint(x + w * 0.5f, y + h * 0.95f));
	BPoint left[3] = {BPoint(x - w * 0.15f, y + h * 0.55f),
		BPoint(x + w * 0.05f, y - h * 0.05f), BPoint(x + w * 0.5f, y + h * 0.28f)};
	shape.BezierTo(left);
	BPoint right[3] = {BPoint(x + w * 0.95f, y - h * 0.05f),
		BPoint(x + w * 1.15f, y + h * 0.55f), BPoint(x + w * 0.5f, y + h * 0.95f)};
	shape.BezierTo(right);
	shape.Close();
	view->MovePenTo(B_ORIGIN);
	if (filled)
		view->FillShape(&shape);
	else
		view->StrokeShape(&shape);
}

}	// namespace ui
