#include "Chrome.h"

#include "Theme.h"

#include <GroupLayout.h>
#include <Window.h>

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

const int kMinTile = 70;
const int kMaxTile = 340;

}	// namespace


// --- Sidebar ----------------------------------------------------------------

SidebarView::SidebarView()
	:
	BView("sidebar", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
	fItem(kItemPhotos),
	fArgument(0),
	fHover(-1),
	fSyncBusy(false),
	fSyncError(false),
	fSyncHover(false)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


BSize
SidebarView::MinSize()
{
	return BSize(S(200), S(200));
}


BSize
SidebarView::MaxSize()
{
	return BSize(S(200), B_SIZE_UNLIMITED);
}


BSize
SidebarView::PreferredSize()
{
	return BSize(S(200), S(600));
}


void
SidebarView::SetRows(const std::vector<SidebarRow>& rows)
{
	fRows = rows;
	Invalidate();
}


void
SidebarView::Select(int item, int64_t argument)
{
	fItem = item;
	fArgument = argument;
	Invalidate();
}


void
SidebarView::SetSync(const char* title, const char* text, bool busy, bool error)
{
	if (fSyncTitle == title && fSyncText == text && fSyncBusy == busy
		&& fSyncError == error)
		return;
	fSyncTitle = title;
	fSyncText = text;
	fSyncBusy = busy;
	fSyncError = error;
	Invalidate(_SyncRect());
}


BRect
SidebarView::_SyncRect() const
{
	BRect bounds = Bounds();
	return BRect(bounds.left, bounds.bottom - S(52), bounds.right, bounds.bottom);
}


int
SidebarView::_RowAt(BPoint where) const
{
	for (size_t i = 0; i < fRects.size() && i < fRows.size(); i++) {
		if (!fRows[i].heading && fRects[i].Contains(where))
			return (int)i;
	}
	return -1;
}


void
SidebarView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	SetDrawingMode(B_OP_COPY);
	SetHighColor(Theme::Sidebar());
	FillRect(updateRect);
	SetHighColor(Theme::Separator());
	StrokeLine(bounds.RightTop(), bounds.RightBottom());

	font_height plain;
	be_plain_font->GetHeight(&plain);
	float rowHeight = ceilf((plain.ascent + plain.descent) * 1.75f);
	float y = S(10);
	fRects.assign(fRows.size(), BRect());
	for (size_t i = 0; i < fRows.size(); i++) {
		const SidebarRow& row = fRows[i];
		if (row.heading) {
			y += S(10);
			BFont font(be_bold_font);
			font.SetSize(be_plain_font->Size() * 0.85f);
			SetFont(&font);
			SetLowColor(Theme::Sidebar());
			SetHighColor(Theme::TertiaryText());
			BString upper(row.label);
			upper.ToUpper();
			DrawString(upper.String(), BPoint(S(16), y + plain.ascent));
			y += rowHeight;
			continue;
		}
		BRect rect(S(8), y, bounds.right - S(8), y + rowHeight - 1);
		fRects[i] = rect;
		bool selected = row.item == fItem && row.argument == fArgument;
		rgb_color background = Theme::Sidebar();
		if (selected)
			background = Theme::Selection();
		else if ((int)i == fHover)
			background = Theme::Hover();
		if (selected || (int)i == fHover) {
			SetHighColor(background);
			FillRoundRect(rect, S(6), S(6));
		}
		SetLowColor(background);
		SetFont(selected ? be_bold_font : be_plain_font);
		SetHighColor(Theme::Text());
		float countWidth = row.count.Length() > 0
			? be_plain_font->StringWidth(row.count.String()) + S(8) : 0;
		float baseline = y + (rowHeight + plain.ascent - plain.descent) / 2;
		DrawTruncated(this, row.label.String(), BPoint(rect.left + S(10), baseline),
			rect.Width() - S(20) - countWidth);
		if (row.count.Length() > 0) {
			SetFont(be_plain_font);
			SetHighColor(Theme::TertiaryText());
			DrawString(row.count.String(), BPoint(rect.right - S(10)
				- be_plain_font->StringWidth(row.count.String()), baseline));
		}
		y += rowHeight + S(1);
	}

	// Immich, at the bottom.
	if (fSyncTitle.Length() > 0) {
		BRect sync = _SyncRect();
		SetHighColor(fSyncHover ? Theme::Hover() : Theme::Sidebar());
		FillRect(sync);
		SetLowColor(fSyncHover ? Theme::Hover() : Theme::Sidebar());
		SetHighColor(Theme::Separator());
		StrokeLine(BPoint(sync.left + S(12), sync.top),
			BPoint(sync.right - S(12), sync.top));
		// A dot for the state.
		rgb_color dot = fSyncError ? Theme::Danger()
			: fSyncBusy ? Theme::Accent()
			: fSyncText.StartsWith("Not connected") ? Theme::TertiaryText()
			: rgb_color{60, 190, 110, 255};
		SetHighColor(dot);
		float middle = sync.top + S(18);
		FillEllipse(BPoint(sync.left + S(21), middle), S(4), S(4));
		BFont font(be_bold_font);
		SetFont(&font);
		SetHighColor(Theme::Text());
		DrawString(fSyncTitle.String(), BPoint(sync.left + S(32),
			middle + (plain.ascent - plain.descent) / 2));
		BFont small(be_plain_font);
		small.SetSize(be_plain_font->Size() * 0.9f);
		SetFont(&small);
		SetHighColor(fSyncError ? Theme::Danger() : Theme::SecondaryText());
		DrawTruncated(this, fSyncText.String(), BPoint(sync.left + S(16),
			middle + S(20) + (plain.ascent - plain.descent) / 2 - S(2)),
			sync.Width() - S(28));
	}
}


void
SidebarView::MouseDown(BPoint where)
{
	if (fSyncTitle.Length() > 0 && _SyncRect().Contains(where)) {
		Window()->PostMessage(kMsgSyncNow);
		return;
	}
	int row = _RowAt(where);
	if (row < 0)
		return;
	BMessage message(kMsgShowView);
	message.AddInt32("item", fRows[row].item);
	message.AddInt64("argument", fRows[row].argument);
	if (fRows[row].key.Length() > 0)
		message.AddString("key", fRows[row].key);
	Window()->PostMessage(&message);
}


void
SidebarView::MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
{
	int hover = transit == B_EXITED_VIEW ? -1 : _RowAt(where);
	bool syncHover = transit != B_EXITED_VIEW && fSyncTitle.Length() > 0
		&& _SyncRect().Contains(where);
	if (hover != fHover || syncHover != fSyncHover) {
		fHover = hover;
		fSyncHover = syncHover;
		Invalidate();
	}
}


// --- Header -----------------------------------------------------------------

HeaderView::HeaderView()
	:
	BView("header", B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
	fShowSegments(false),
	fSegment(kGroupDays),
	fShowZoom(false),
	fTileSize(150),
	fHover(-1),
	fZoomDrag(false)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


BSize
HeaderView::MinSize()
{
	return BSize(S(300), S(50));
}


BSize
HeaderView::MaxSize()
{
	return BSize(B_SIZE_UNLIMITED, S(50));
}


BSize
HeaderView::PreferredSize()
{
	return BSize(S(800), S(50));
}


void
HeaderView::Clear()
{
	fBack = "";
	fTitle = "";
	fSubtitle = "";
	fShowSegments = false;
	fShowZoom = false;
	fButtons.clear();
	fHover = -1;
}


void
HeaderView::SetBack(const char* label)
{
	fBack = label != nullptr ? label : "";
}


void
HeaderView::SetTitle(const char* title, const char* subtitle)
{
	fTitle = title;
	fSubtitle = subtitle != nullptr ? subtitle : "";
}


void
HeaderView::SetSegments(bool show, int selected)
{
	fShowSegments = show;
	fSegment = selected;
}


void
HeaderView::SetZoom(bool show, int tileSize)
{
	fShowZoom = show;
	fTileSize = tileSize;
}


void
HeaderView::AddButton(const char* label, uint32 what, uint32 flags)
{
	Button button;
	button.label = label;
	button.what = what;
	button.flags = flags;
	fButtons.push_back(button);
}


void
HeaderView::Finish()
{
	_Layout();
	Invalidate();
}


void
HeaderView::_Layout()
{
	BRect bounds = Bounds();
	font_height plain;
	be_plain_font->GetHeight(&plain);
	float buttonHeight = ceilf((plain.ascent + plain.descent) * 1.9f);
	float top = floorf((bounds.Height() - buttonHeight) / 2);
	float padding = S(11);

	// From the right: buttons, the size slider.
	float x = bounds.right - S(12);
	for (int i = (int)fButtons.size() - 1; i >= 0; i--) {
		Button& button = fButtons[i];
		if ((button.flags & kButtonLeftSide) != 0)
			continue;
		float width = (button.flags & kButtonHeart) != 0 ? buttonHeight
			: be_plain_font->StringWidth(button.label.String()) + 2 * padding;
		button.rect = BRect(x - width, top, x, top + buttonHeight);
		x -= width + S(6);
	}
	fZoomRect = BRect();
	if (fShowZoom) {
		float width = S(110);
		fZoomRect = BRect(x - width - S(8), top, x - S(8), top + buttonHeight);
		x -= width + S(22);
	}

	// From the left: the way back, then the title (drawn), then buttons that
	// belong to it.
	float leftX = S(14);
	fBackRect = BRect();
	if (fBack.Length() > 0) {
		BString label("‹  ");
		label << fBack;
		float width = be_plain_font->StringWidth(label.String()) + 2 * padding;
		fBackRect = BRect(leftX, top, leftX + width, top + buttonHeight);
	}

	// The segments sit in the middle when there is room, else after the title.
	for (int i = 0; i < 4; i++)
		fSegmentRects[i] = BRect();
	if (fShowSegments) {
		static const char* const kLabels[] = {"Years", "Months", "Days", "All Photos"};
		float widths[4], total = 0;
		for (int i = 0; i < 4; i++) {
			widths[i] = be_plain_font->StringWidth(kLabels[i]) + 2 * padding;
			total += widths[i];
		}
		float start = floorf(bounds.left + (bounds.Width() - total) / 2);
		if (start + total > x - S(10))
			start = x - S(10) - total;
		for (int i = 0; i < 4; i++) {
			fSegmentRects[i] = BRect(start, top, start + widths[i] - 1,
				top + buttonHeight);
			start += widths[i];
		}
	}
}


void
HeaderView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	SetDrawingMode(B_OP_COPY);
	SetHighColor(Theme::Header());
	FillRect(updateRect);
	SetLowColor(Theme::Header());
	SetHighColor(Theme::Separator());
	StrokeLine(bounds.LeftBottom(), bounds.RightBottom());

	font_height plain;
	be_plain_font->GetHeight(&plain);
	auto baselineIn = [&](BRect rect) {
		return rect.top + (rect.Height() + plain.ascent - plain.descent) / 2;
	};

	float titleLeft = S(18);
	if (fBackRect.IsValid()) {
		BString label("‹  ");
		label << fBack;
		if (fHover == -2) {
			SetHighColor(Theme::Control());
			FillRoundRect(fBackRect, S(7), S(7));
			SetLowColor(Theme::Control());
		}
		SetFont(be_plain_font);
		SetHighColor(Theme::Accent());
		DrawString(label.String(), BPoint(fBackRect.left + S(11), baselineIn(fBackRect)));
		SetLowColor(Theme::Header());
		titleLeft = fBackRect.right + S(12);
	}

	// Title and subtitle, cut where the next thing begins.
	float titleRight = bounds.right - S(12);
	for (auto& button : fButtons) {
		if (button.rect.IsValid())
			titleRight = std::min(titleRight, button.rect.left - S(10));
	}
	if (fZoomRect.IsValid())
		titleRight = std::min(titleRight, fZoomRect.left - S(16));
	if (fSegmentRects[0].IsValid())
		titleRight = std::min(titleRight, fSegmentRects[0].left - S(12));
	BFont titleFont(be_bold_font);
	titleFont.SetSize(be_plain_font->Size() * 1.25f);
	SetFont(&titleFont);
	SetHighColor(Theme::Text());
	font_height titleHeight;
	titleFont.GetHeight(&titleHeight);
	float titleBaseline = fSubtitle.Length() > 0
		? bounds.top + bounds.Height() / 2 - S(1)
		: bounds.top + (bounds.Height() + titleHeight.ascent - titleHeight.descent) / 2;
	if (titleRight > titleLeft + S(30)) {
		DrawTruncated(this, fTitle.String(), BPoint(titleLeft, titleBaseline),
			titleRight - titleLeft);
		if (fSubtitle.Length() > 0) {
			BFont small(be_plain_font);
			small.SetSize(be_plain_font->Size() * 0.9f);
			SetFont(&small);
			SetHighColor(Theme::SecondaryText());
			DrawTruncated(this, fSubtitle.String(),
				BPoint(titleLeft, titleBaseline + plain.ascent + S(4)),
				titleRight - titleLeft);
		}
	}

	if (fShowSegments && fSegmentRects[0].IsValid()) {
		static const char* const kLabels[] = {"Years", "Months", "Days", "All Photos"};
		BRect all(fSegmentRects[0].left, fSegmentRects[0].top,
			fSegmentRects[3].right, fSegmentRects[0].bottom);
		SetHighColor(Theme::Control());
		FillRoundRect(all, S(8), S(8));
		for (int i = 0; i < 4; i++) {
			bool selected = i == fSegment;
			if (selected) {
				SetHighColor(Theme::ControlSelected());
				FillRoundRect(fSegmentRects[i].InsetByCopy(S(2), S(2)), S(6), S(6));
				SetLowColor(Theme::ControlSelected());
			} else
				SetLowColor(Theme::Control());
			SetFont(selected ? be_bold_font : be_plain_font);
			const BFont* font = selected ? be_bold_font : be_plain_font;
			SetHighColor(selected ? Theme::Text() : Theme::SecondaryText());
			DrawString(kLabels[i], BPoint(fSegmentRects[i].left
				+ (fSegmentRects[i].Width() - font->StringWidth(kLabels[i])) / 2,
				baselineIn(fSegmentRects[i])));
		}
		SetLowColor(Theme::Header());
	}

	if (fZoomRect.IsValid()) {
		// Small tile, track with knob, large tile.
		float middle = floorf(fZoomRect.top + fZoomRect.Height() / 2);
		BRect track(fZoomRect.left + S(14), middle - S(1), fZoomRect.right - S(18),
			middle + S(1));
		SetHighColor(Theme::TertiaryText());
		FillRect(BRect(fZoomRect.left, middle - S(3), fZoomRect.left + S(6),
			middle + S(3)));
		FillRect(BRect(fZoomRect.right - S(11), middle - S(5), fZoomRect.right,
			middle + S(5)));
		SetHighColor(Theme::Control());
		FillRoundRect(track.InsetByCopy(0, -S(1)), S(2), S(2));
		float fraction = (float)(fTileSize - S(kMinTile)) / (S(kMaxTile) - S(kMinTile));
		fraction = std::max(0.0f, std::min(1.0f, fraction));
		float knob = track.left + fraction * track.Width();
		SetHighColor(Theme::Accent());
		FillRoundRect(BRect(track.left, track.top - S(1), knob, track.bottom + S(1)),
			S(2), S(2));
		SetHighColor(Theme::IsDark() ? rgb_color{230, 230, 235, 255}
			: rgb_color{255, 255, 255, 255});
		FillEllipse(BPoint(knob, middle), S(7), S(7));
		SetHighColor(Theme::TertiaryText());
		StrokeEllipse(BPoint(knob, middle), S(7), S(7));
	}

	for (size_t i = 0; i < fButtons.size(); i++) {
		const Button& button = fButtons[i];
		if (!button.rect.IsValid())
			continue;
		bool disabled = (button.flags & kButtonDisabled) != 0;
		bool hover = (int)i == fHover && !disabled;
		if ((button.flags & kButtonHeart) != 0) {
			if (hover) {
				SetHighColor(Theme::Control());
				FillRoundRect(button.rect, S(7), S(7));
			}
			bool on = (button.flags & kButtonOn) != 0;
			SetHighColor(on ? Theme::Favorite() : Theme::SecondaryText());
			float size = button.rect.Height() * 0.5f;
			BRect heart(button.rect.left + (button.rect.Width() - size) / 2,
				button.rect.top + (button.rect.Height() - size) / 2, 0, 0);
			heart.right = heart.left + size;
			heart.bottom = heart.top + size;
			SetPenSize(S(1.5f));
			DrawHeart(this, heart, on);
			SetPenSize(1);
			continue;
		}
		rgb_color fill = Theme::Control();
		rgb_color text = Theme::Text();
		if ((button.flags & kButtonAccent) != 0) {
			fill = Theme::Accent();
			text = Theme::AccentText();
		} else if ((button.flags & kButtonOn) != 0) {
			fill = Theme::Mix(Theme::Control(), Theme::Accent(), 0.35f);
		}
		if ((button.flags & kButtonDanger) != 0)
			text = Theme::Danger();
		if (hover)
			fill = Theme::Mix(fill, Theme::IsDark() ? rgb_color{255, 255, 255, 255}
				: rgb_color{0, 0, 0, 255}, 0.08f);
		if (disabled)
			text = Theme::TertiaryText();
		SetHighColor(fill);
		FillRoundRect(button.rect, S(7), S(7));
		SetLowColor(fill);
		SetFont(be_plain_font);
		SetHighColor(text);
		DrawString(button.label.String(), BPoint(button.rect.left
			+ (button.rect.Width() - be_plain_font->StringWidth(button.label.String())) / 2,
			baselineIn(button.rect)));
		SetLowColor(Theme::Header());
	}
}


void
HeaderView::_SetZoomFrom(float x)
{
	float left = fZoomRect.left + S(14), right = fZoomRect.right - S(18);
	float fraction = std::max(0.0f, std::min(1.0f, (x - left) / (right - left)));
	int size = (int)(S(kMinTile) + fraction * (S(kMaxTile) - S(kMinTile)));
	// Steps of 8: fewer sizes of tiles to make while dragging.
	size = (size / 8) * 8;
	if (size == fTileSize)
		return;
	fTileSize = size;
	Invalidate(fZoomRect.InsetByCopy(-S(10), -S(4)));
	BMessage message(kMsgTileSize);
	message.AddInt32("size", size);
	Window()->PostMessage(&message);
}


void
HeaderView::MouseDown(BPoint where)
{
	if (fBackRect.IsValid() && fBackRect.Contains(where)) {
		Window()->PostMessage(kMsgCloseViewer);
		return;
	}
	if (fShowSegments) {
		for (int i = 0; i < 4; i++) {
			if (fSegmentRects[i].IsValid() && fSegmentRects[i].Contains(where)) {
				BMessage message(kMsgGroupMode);
				message.AddInt32("mode", i);
				Window()->PostMessage(&message);
				return;
			}
		}
	}
	if (fZoomRect.IsValid() && fZoomRect.InsetByCopy(-S(6), 0).Contains(where)) {
		fZoomDrag = true;
		SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
		_SetZoomFrom(where.x);
		return;
	}
	for (auto& button : fButtons) {
		if (button.rect.IsValid() && button.rect.Contains(where)
			&& (button.flags & kButtonDisabled) == 0) {
			Window()->PostMessage(button.what);
			return;
		}
	}
}


void
HeaderView::MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
{
	if (fZoomDrag) {
		_SetZoomFrom(where.x);
		return;
	}
	int hover = -1;
	if (transit != B_EXITED_VIEW) {
		if (fBackRect.IsValid() && fBackRect.Contains(where))
			hover = -2;
		for (size_t i = 0; i < fButtons.size(); i++) {
			if (fButtons[i].rect.IsValid() && fButtons[i].rect.Contains(where))
				hover = (int)i;
		}
	}
	if (hover != fHover) {
		fHover = hover;
		Invalidate();
	}
}


void
HeaderView::MouseUp(BPoint where)
{
	fZoomDrag = false;
}


// --- ScrollBox --------------------------------------------------------------

class ScrollBox::Clip : public BView {
public:
	Clip()
		:
		BView("clip", B_FRAME_EVENTS)
	{
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		SetExplicitMinSize(BSize(S(100), S(80)));
		SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
	}

	virtual void FrameResized(float width, float height)
	{
		ScrollBox* box = dynamic_cast<ScrollBox*>(Parent());
		if (box != nullptr)
			box->FrameResized(0, 0);
	}
};


ScrollBox::ScrollBox(const char* name, BView* content, float width)
	:
	BView(name, B_FRAME_EVENTS),
	fContent(content)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	fClip = new Clip();
	fBar = new BScrollBar("bar", fClip, 0, 0, B_VERTICAL);
	BGroupLayout* layout = new BGroupLayout(B_HORIZONTAL, 0);
	SetLayout(layout);
	layout->AddView(fClip);
	layout->AddView(fBar);
	fClip->AddChild(fContent);
	SetExplicitMinSize(BSize(width, S(120)));
	SetExplicitMaxSize(BSize(width, B_SIZE_UNLIMITED));
	SetExplicitPreferredSize(BSize(width, S(500)));
}


void
ScrollBox::AttachedToWindow()
{
	BView::AttachedToWindow();
	_Update();
}


void
ScrollBox::FrameResized(float width, float height)
{
	_Update();
}


void
ScrollBox::_Update()
{
	BRect clip = fClip->Bounds();
	float minimum = fContent->MinSize().height;
	float height = std::max(clip.Height(), minimum);
	fContent->MoveTo(0, 0);
	fContent->ResizeTo(clip.Width(), height);
	float range = std::max(0.0f, height - clip.Height());
	fBar->SetRange(0, range);
	fBar->SetProportion(height > 0 ? std::min(1.0f, (clip.Height() + 1) / (height + 1)) : 1);
	fBar->SetSteps(S(20), std::max(S(20), clip.Height() - S(30)));
	if (clip.top > range)
		fClip->ScrollTo(0, range);
}


// --- Status line ------------------------------------------------------------

StatusView::StatusView()
	:
	BView("status", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
	fProgress(-1)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


BSize
StatusView::MinSize()
{
	return BSize(S(200), S(26));
}


BSize
StatusView::MaxSize()
{
	return BSize(B_SIZE_UNLIMITED, S(26));
}


BSize
StatusView::PreferredSize()
{
	return BSize(S(800), S(26));
}


void
StatusView::SetLeft(const char* text)
{
	if (fLeft == text)
		return;
	fLeft = text;
	Invalidate();
}


void
StatusView::SetRight(const char* text, float progress)
{
	if (fRight == text && fabsf(progress - fProgress) < 0.002f)
		return;
	fRight = text;
	fProgress = progress;
	Invalidate();
}


void
StatusView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	SetDrawingMode(B_OP_COPY);
	SetHighColor(Theme::Header());
	FillRect(updateRect);
	SetLowColor(Theme::Header());
	SetHighColor(Theme::Separator());
	StrokeLine(bounds.LeftTop(), bounds.RightTop());
	BFont font(be_plain_font);
	font.SetSize(be_plain_font->Size() * 0.92f);
	SetFont(&font);
	font_height height;
	font.GetHeight(&height);
	float baseline = bounds.top + (bounds.Height() + height.ascent - height.descent) / 2 + 1;
	SetHighColor(Theme::SecondaryText());
	float rightWidth = font.StringWidth(fRight.String());
	float barWidth = fProgress >= 0 ? S(120) : 0;
	DrawTruncated(this, fLeft.String(), BPoint(S(14), baseline),
		bounds.Width() - rightWidth - barWidth - S(60));
	float right = bounds.right - S(14);
	if (fProgress >= 0) {
		BRect bar(right - barWidth, bounds.top + bounds.Height() / 2 - S(2),
			right, bounds.top + bounds.Height() / 2 + S(2));
		SetHighColor(Theme::Control());
		FillRoundRect(bar, S(2), S(2));
		BRect done = bar;
		done.right = bar.left + bar.Width() * std::min(1.0f, fProgress);
		SetHighColor(Theme::Accent());
		FillRoundRect(done, S(2), S(2));
		right = bar.left - S(10);
	}
	SetHighColor(Theme::SecondaryText());
	DrawString(fRight.String(), BPoint(right - rightWidth, baseline));
}

}	// namespace ui
