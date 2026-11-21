#include "GridView.h"

#include "Theme.h"

#include <Bitmap.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ScrollBar.h>
#include <Window.h>

#include <algorithm>

using namespace photos;

namespace ui {

namespace {

// How many rows of a section the year and month views show.
const int kYearRows = 2;
const int kMonthRows = 3;

}	// namespace


GridView::GridView(Session& session)
	:
	BView("grid", B_WILL_DRAW | B_FRAME_EVENTS | B_NAVIGABLE
		| B_FULL_UPDATE_ON_RESIZE),
	fSession(session),
	fMode(kGroupDays),
	fSummaries(true),
	fTile((int)S(150)),
	fColumns(1),
	fGap(S(3)),
	fMargin(S(18)),
	fLeft(0),
	fHeaderHeight(S(46)),
	fTotalHeight(0),
	fFocus(-1),
	fAnchor(-1)
{
	SetViewColor(B_TRANSPARENT_COLOR);
	// A view without a layout would ask for the size it happens to have.
	SetExplicitMinSize(BSize(S(240), S(160)));
	SetExplicitPreferredSize(BSize(S(700), S(500)));
	SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
}


void
GridView::AttachedToWindow()
{
	BView::AttachedToWindow();
	_Layout();
}


void
GridView::SetEmptyText(const char* title, const char* text)
{
	fEmptyTitle = title;
	fEmptyText = text;
	if (fPhotos.empty())
		Invalidate();
}


void
GridView::SetPhotos(std::vector<PhotoBrief>& photos, GroupMode mode,
	bool keepPosition)
{
	// What the view shows now, to find it again in the new list.
	int64_t focusId = fFocus >= 0 && fFocus < (int)fCells.size()
		? fPhotos[fCells[fFocus].photo].id : 0;
	int64_t topId = 0;
	float topOffset = 0;
	if (keepPosition && !fCells.empty()) {
		float y = Bounds().top;
		int section = _SectionAt(y);
		if (section >= 0) {
			const Section& s = fSections[section];
			int cell = s.first;
			if (y > s.tilesTop && s.count > 0) {
				int row = (int)((y - s.tilesTop) / (fTile + fGap));
				cell = std::min(s.first + row * fColumns, s.first + s.count - 1);
			}
			if (cell < (int)fCells.size()) {
				topId = fPhotos[fCells[cell].photo].id;
				topOffset = y - _CellRect(cell).top;
			}
		}
	}

	fPhotos.swap(photos);
	fMode = mode;
	fIndex.clear();
	for (size_t i = 0; i < fPhotos.size(); i++)
		fIndex[fPhotos[i].id] = (int)i;
	// A selection survives as far as its photos do.
	for (auto it = fSelected.begin(); it != fSelected.end();) {
		if (fIndex.count(*it) == 0)
			it = fSelected.erase(it);
		else
			++it;
	}
	fSession.Pics().ForgetQueued();
	_Build();
	_Layout();

	fFocus = fAnchor = -1;
	if (focusId != 0) {
		auto found = fIndex.find(focusId);
		if (found != fIndex.end()) {
			auto cell = fCellOfPhoto.find(found->second);
			if (cell != fCellOfPhoto.end())
				fFocus = fAnchor = cell->second;
		}
	}
	float y = 0;
	if (topId != 0) {
		auto found = fIndex.find(topId);
		if (found != fIndex.end()) {
			auto cell = fCellOfPhoto.find(found->second);
			if (cell != fCellOfPhoto.end())
				y = _CellRect(cell->second).top + topOffset;
		}
	}
	float maximum = std::max(0.0f, fTotalHeight - Bounds().Height());
	BView::ScrollTo(BPoint(0, std::max(0.0f, std::min(y, maximum))));
	_UpdateScrollBar();
	Invalidate();
	_SelectionChanged();
}


void
GridView::SetGroupMode(GroupMode mode)
{
	if (mode == fMode)
		return;
	std::vector<PhotoBrief> photos = fPhotos;
	SetPhotos(photos, mode, true);
}


void
GridView::SetTileSize(int size)
{
	size = std::max((int)S(60), std::min(size, (int)S(400)));
	if (size == fTile)
		return;
	// Keep what is in the middle of the view in the middle.
	int middle = _CellAt(BPoint(fLeft + fTile / 2,
		Bounds().top + Bounds().Height() / 2));
	fTile = size;
	fSession.Pics().ForgetQueued();
	_Build();
	_Layout();
	if (middle >= 0 && middle < (int)fCells.size()) {
		float y = _CellRect(middle).top - Bounds().Height() / 2 + fTile / 2;
		float maximum = std::max(0.0f, fTotalHeight - Bounds().Height());
		BView::ScrollTo(BPoint(0, std::max(0.0f, std::min(y, maximum))));
	}
	_UpdateScrollBar();
	Invalidate();
}


void
GridView::_Build()
{
	fSections.clear();
	fCells.clear();
	fCellOfPhoto.clear();
	if (fPhotos.empty())
		return;
	float width = Bounds().Width() - 2 * fMargin;
	fColumns = std::max(1, (int)((width + fGap) / (fTile + fGap)));

	// The sections' ranges of photos.
	struct Range {
		int first;
		int count;
		int64_t key;
	};
	std::vector<Range> ranges;
	if (fMode == kGroupNone) {
		Range range = {0, (int)fPhotos.size(), 0};
		ranges.push_back(range);
	} else {
		for (size_t i = 0; i < fPhotos.size(); i++) {
			CivilTime time = SecondsToCivil(fPhotos[i].taken);
			int64_t key = time.year;
			if (fMode != kGroupYears)
				key = key * 100 + time.month;
			if (fMode == kGroupDays)
				key = key * 100 + time.day;
			if (fPhotos[i].taken == 0)
				key = 0;
			if (ranges.empty() || ranges.back().key != key) {
				Range range = {(int)i, 0, key};
				ranges.push_back(range);
			}
			ranges.back().count++;
		}
	}

	bool summary = fSummaries && (fMode == kGroupYears || fMode == kGroupMonths);
	int maxCells = summary
		? fColumns * (fMode == kGroupYears ? kYearRows : kMonthRows) : INT32_MAX;
	for (auto& range : ranges) {
		Section section;
		section.first = (int)fCells.size();
		section.total = range.count;
		section.time = fPhotos[range.first].taken;
		if (fMode != kGroupNone) {
			if (range.key == 0)
				section.title = "No date";
			else if (fMode == kGroupYears)
				section.title.SetToFormat("%d", (int)range.key);
			else if (fMode == kGroupMonths)
				section.title = MonthTitle(section.time);
			else
				section.title = DayTitle(section.time);
			section.subtitle = CountText(range.count, "photo", "photos");
		}
		int shown = std::min(range.count, maxCells);
		for (int i = 0; i < shown; i++) {
			// A summary takes photos from all over the section.
			int offset = shown == range.count || shown <= 1 ? i
				: (int)((int64_t)i * (range.count - 1) / (shown - 1));
			Cell cell = {range.first + offset, (int)fSections.size()};
			fCellOfPhoto[cell.photo] = (int)fCells.size();
			fCells.push_back(cell);
		}
		section.count = shown;
		fSections.push_back(section);
	}
}


void
GridView::_Layout()
{
	float width = Bounds().Width() - 2 * fMargin;
	int columns = std::max(1, (int)((width + fGap) / (fTile + fGap)));
	if (columns != fColumns && !fPhotos.empty()) {
		fColumns = columns;
		// Summaries depend on the number of columns.
		if (fSummaries && (fMode == kGroupYears || fMode == kGroupMonths)) {
			int64_t focusPhoto = fFocus >= 0 && fFocus < (int)fCells.size()
				? fCells[fFocus].photo : -1;
			_Build();
			auto found = fCellOfPhoto.find((int)focusPhoto);
			fFocus = found != fCellOfPhoto.end() ? found->second : -1;
			fAnchor = fFocus;
		}
	}
	fColumns = columns;
	float used = fColumns * fTile + (fColumns - 1) * fGap;
	fLeft = fMargin + std::max(0.0f, floorf((width - used) / 2));
	float y = S(6);
	for (auto& section : fSections) {
		section.top = y;
		if (fMode != kGroupNone)
			y += fHeaderHeight;
		else
			y += S(10);
		section.tilesTop = y;
		int rows = (section.count + fColumns - 1) / fColumns;
		y += rows * (fTile + fGap);
		section.bottom = y;
		y += S(10);
	}
	fTotalHeight = y + S(12);
	_UpdateScrollBar();
}


void
GridView::_UpdateScrollBar()
{
	BScrollBar* bar = ScrollBar(B_VERTICAL);
	if (bar == nullptr)
		return;
	float height = Bounds().Height();
	float maximum = std::max(0.0f, fTotalHeight - height);
	bar->SetRange(0, maximum);
	bar->SetProportion(fTotalHeight > 0 ? std::min(1.0f, height / fTotalHeight) : 1);
	bar->SetSteps(fTile + fGap, std::max(fTile + fGap, height - fTile));
	if (Bounds().top > maximum)
		BView::ScrollTo(BPoint(0, maximum));
}


void
GridView::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	_Layout();
	Invalidate();
}


void
GridView::ScrollTo(BPoint where)
{
	BView::ScrollTo(where);
}


void
GridView::MakeFocus(bool focus)
{
	BView::MakeFocus(focus);
	if (fFocus >= 0)
		_InvalidateCell(fFocus);
}


BRect
GridView::_CellRect(int cell) const
{
	const Section& section = fSections[fCells[cell].section];
	int position = cell - section.first;
	int row = position / fColumns;
	int column = position % fColumns;
	float left = fLeft + column * (fTile + fGap);
	float top = section.tilesTop + row * (fTile + fGap);
	return BRect(left, top, left + fTile - 1, top + fTile - 1);
}


int
GridView::_SectionAt(float y) const
{
	if (fSections.empty())
		return -1;
	int low = 0, high = (int)fSections.size() - 1;
	while (low < high) {
		int middle = (low + high) / 2;
		if (fSections[middle].bottom + S(10) <= y)
			low = middle + 1;
		else
			high = middle;
	}
	return low;
}


int
GridView::_CellAt(BPoint where) const
{
	int index = _SectionAt(where.y);
	if (index < 0)
		return -1;
	const Section& section = fSections[index];
	if (where.y < section.tilesTop || where.y >= section.bottom
		|| where.x < fLeft)
		return -1;
	int column = (int)((where.x - fLeft) / (fTile + fGap));
	int row = (int)((where.y - section.tilesTop) / (fTile + fGap));
	if (column >= fColumns)
		return -1;
	// Not in the gaps.
	if (where.x - fLeft - column * (fTile + fGap) >= fTile
		|| where.y - section.tilesTop - row * (fTile + fGap) >= fTile)
		return -1;
	int position = row * fColumns + column;
	if (position >= section.count)
		return -1;
	return section.first + position;
}


int
GridView::_HeaderAt(BPoint where) const
{
	if (fMode == kGroupNone)
		return -1;
	int index = _SectionAt(where.y);
	if (index < 0)
		return -1;
	const Section& section = fSections[index];
	return where.y >= section.top && where.y < section.tilesTop ? index : -1;
}


void
GridView::_InvalidateCell(int cell)
{
	if (cell >= 0 && cell < (int)fCells.size())
		Invalidate(_CellRect(cell).InsetByCopy(-S(3), -S(3)));
}


void
GridView::TileReady(int64_t id)
{
	auto found = fIndex.find(id);
	if (found == fIndex.end())
		return;
	auto cell = fCellOfPhoto.find(found->second);
	if (cell == fCellOfPhoto.end())
		return;
	BRect rect = _CellRect(cell->second);
	if (rect.Intersects(Bounds()))
		Invalidate(rect);
}


void
GridView::_DrawCell(int cell, BRect rect)
{
	const PhotoBrief& photo = fPhotos[fCells[cell].photo];
	std::shared_ptr<BBitmap> bitmap = fSession.Pics().Tile(photo.id,
		photo.revision, fTile);
	if (bitmap) {
		SetDrawingMode(B_OP_COPY);
		DrawBitmap(bitmap.get(), rect.LeftTop());
	} else {
		SetHighColor(Theme::Placeholder());
		FillRect(rect);
	}

	bool selected = fSelected.count(photo.id) != 0;
	if (photo.favorite) {
		float size = std::max(S(13), fTile * 0.11f);
		BRect heart(rect.left + S(6), rect.bottom - S(6) - size,
			rect.left + S(6) + size, rect.bottom - S(6));
		SetDrawingMode(B_OP_ALPHA);
		SetHighColor(0, 0, 0, 90);
		DrawHeart(this, heart.OffsetByCopy(0, 1), true);
		SetHighColor(255, 255, 255, 255);
		DrawHeart(this, heart, true);
		SetDrawingMode(B_OP_COPY);
	}

	// A summary's last tile tells how many more there are.
	const Section& section = fSections[fCells[cell].section];
	if (section.total > section.count && cell == section.first + section.count - 1) {
		SetDrawingMode(B_OP_ALPHA);
		SetHighColor(0, 0, 0, 120);
		FillRect(rect);
		BString more;
		more << "+" << Number(section.total - section.count);
		BFont font(be_bold_font);
		font.SetSize(std::max(be_plain_font->Size(), fTile * 0.16f));
		SetFont(&font);
		SetHighColor(255, 255, 255, 255);
		font_height height;
		font.GetHeight(&height);
		DrawString(more.String(), BPoint(
			rect.left + (rect.Width() - font.StringWidth(more.String())) / 2,
			rect.top + (rect.Height() + height.ascent - height.descent) / 2));
		SetDrawingMode(B_OP_COPY);
	}

	if (selected) {
		SetHighColor(Theme::Accent());
		SetPenSize(S(3));
		StrokeRect(rect.InsetByCopy(S(1), S(1)));
		SetPenSize(1);
		// A check mark in the corner.
		float size = S(20);
		BRect badge(rect.right - size - S(6), rect.top + S(6), rect.right - S(6),
			rect.top + S(6) + size);
		FillEllipse(badge);
		SetHighColor(255, 255, 255);
		SetPenSize(S(2));
		BPoint a(badge.left + size * 0.27f, badge.top + size * 0.52f);
		BPoint b(badge.left + size * 0.44f, badge.top + size * 0.69f);
		BPoint c(badge.left + size * 0.74f, badge.top + size * 0.33f);
		StrokeLine(a, b);
		StrokeLine(b, c);
		SetPenSize(1);
	} else if (cell == fFocus && IsFocus() && Window()->IsActive()) {
		SetHighColor(Theme::Mix(Theme::Accent(), Theme::Background(), 0.35f));
		SetPenSize(S(2));
		StrokeRect(rect.InsetByCopy(S(1), S(1)));
		SetPenSize(1);
	}
}


void
GridView::Draw(BRect updateRect)
{
	SetDrawingMode(B_OP_COPY);
	SetHighColor(Theme::Background());
	FillRect(updateRect);
	SetLowColor(Theme::Background());

	if (fPhotos.empty()) {
		BRect bounds = Bounds();
		BFont title(be_bold_font);
		title.SetSize(be_plain_font->Size() * 1.5f);
		SetFont(&title);
		SetHighColor(Theme::SecondaryText());
		float y = bounds.top + bounds.Height() * 0.42f;
		DrawString(fEmptyTitle.String(), BPoint(bounds.left
			+ (bounds.Width() - title.StringWidth(fEmptyTitle.String())) / 2, y));
		SetFont(be_plain_font);
		SetHighColor(Theme::TertiaryText());
		// The text may have several lines.
		BString text = fEmptyText;
		float line = be_plain_font->Size() * 1.6f;
		y += line * 1.4f;
		while (text.Length() > 0) {
			int32 end = text.FindFirst('\n');
			BString part = end >= 0 ? BString(text.String(), end) : text;
			DrawString(part.String(), BPoint(bounds.left
				+ (bounds.Width() - be_plain_font->StringWidth(part.String())) / 2, y));
			y += line;
			if (end < 0)
				break;
			text.Remove(0, end + 1);
		}
		return;
	}

	int first = _SectionAt(updateRect.top);
	if (first < 0)
		return;
	for (int index = first; index < (int)fSections.size(); index++) {
		const Section& section = fSections[index];
		if (section.top > updateRect.bottom)
			break;
		if (fMode != kGroupNone && section.tilesTop >= updateRect.top) {
			BFont font(be_bold_font);
			font.SetSize(be_plain_font->Size() * (fMode == kGroupDays ? 1.1f : 1.35f));
			SetFont(&font);
			font_height height;
			font.GetHeight(&height);
			SetHighColor(Theme::Text());
			float baseline = section.tilesTop - S(12);
			DrawString(section.title.String(), BPoint(fLeft, baseline));
			float x = fLeft + font.StringWidth(section.title.String()) + S(10);
			SetFont(be_plain_font);
			SetHighColor(Theme::TertiaryText());
			DrawString(section.subtitle.String(), BPoint(x, baseline));
			if (fSummaries && (fMode == kGroupYears || fMode == kGroupMonths)) {
				// The way into the section.
				const char* more = fMode == kGroupYears ? "Months ›" : "Days ›";
				SetHighColor(Theme::Accent());
				float right = fLeft + fColumns * (fTile + fGap) - fGap;
				DrawString(more, BPoint(right - be_plain_font->StringWidth(more),
					baseline));
			}
		}
		if (section.count == 0)
			continue;
		// The rows of the section that are in the update rectangle.
		int firstRow = std::max(0,
			(int)((updateRect.top - section.tilesTop) / (fTile + fGap)));
		int lastRow = (int)((updateRect.bottom - section.tilesTop) / (fTile + fGap));
		int rows = (section.count + fColumns - 1) / fColumns;
		lastRow = std::min(lastRow, rows - 1);
		for (int row = firstRow; row <= lastRow; row++) {
			for (int column = 0; column < fColumns; column++) {
				int position = row * fColumns + column;
				if (position >= section.count)
					break;
				int cell = section.first + position;
				BRect rect = _CellRect(cell);
				if (rect.Intersects(updateRect))
					_DrawCell(cell, rect);
			}
		}
	}
}


std::vector<int64_t>
GridView::Selection() const
{
	// In the order of the list.
	std::vector<int64_t> ids;
	if (fSelected.size() > 64) {
		for (auto& photo : fPhotos) {
			if (fSelected.count(photo.id) != 0)
				ids.push_back(photo.id);
		}
	} else {
		std::vector<std::pair<int, int64_t>> ordered;
		for (int64_t id : fSelected) {
			auto found = fIndex.find(id);
			if (found != fIndex.end())
				ordered.emplace_back(found->second, id);
		}
		std::sort(ordered.begin(), ordered.end());
		for (auto& pair : ordered)
			ids.push_back(pair.second);
	}
	return ids;
}


void
GridView::_SelectionChanged()
{
	if (Window() != nullptr)
		Window()->PostMessage(kMsgSelectionChanged);
}


void
GridView::SelectAll()
{
	for (auto& photo : fPhotos)
		fSelected.insert(photo.id);
	Invalidate();
	_SelectionChanged();
}


void
GridView::ClearSelection()
{
	if (fSelected.empty())
		return;
	fSelected.clear();
	Invalidate();
	_SelectionChanged();
}


int
GridView::IndexOf(int64_t id) const
{
	auto found = fIndex.find(id);
	return found != fIndex.end() ? found->second : -1;
}


void
GridView::Select(int64_t id, bool scrollTo)
{
	auto found = fIndex.find(id);
	if (found == fIndex.end())
		return;
	fSelected.clear();
	fSelected.insert(id);
	auto cell = fCellOfPhoto.find(found->second);
	if (cell != fCellOfPhoto.end()) {
		fFocus = fAnchor = cell->second;
		if (scrollTo)
			_EnsureVisible(cell->second);
	}
	Invalidate();
	_SelectionChanged();
}


void
GridView::ScrollToTime(int64_t taken)
{
	// The list runs from new to old: the first section that starts at or
	// before the time.
	for (auto& section : fSections) {
		if (section.time <= taken || &section == &fSections.back()) {
			float maximum = std::max(0.0f, fTotalHeight - Bounds().Height());
			BView::ScrollTo(BPoint(0, std::min(maximum,
				std::max(0.0f, section.top - S(4)))));
			_UpdateScrollBar();
			break;
		}
	}
}


void
GridView::_EnsureVisible(int cell)
{
	if (cell < 0 || cell >= (int)fCells.size())
		return;
	BRect rect = _CellRect(cell);
	BRect bounds = Bounds();
	float y = bounds.top;
	if (rect.top < bounds.top + S(4))
		y = rect.top - S(8) - (fMode != kGroupNone
			&& cell < fSections[fCells[cell].section].first + fColumns
				? fHeaderHeight : 0);
	else if (rect.bottom > bounds.bottom - S(4))
		y = rect.bottom + S(8) - bounds.Height();
	float maximum = std::max(0.0f, fTotalHeight - bounds.Height());
	y = std::max(0.0f, std::min(y, maximum));
	if (y != bounds.top)
		BView::ScrollTo(BPoint(0, y));
}


void
GridView::_SetFocusCell(int cell, bool extend, bool keep)
{
	if (cell < 0 || cell >= (int)fCells.size())
		return;
	int64_t id = fPhotos[fCells[cell].photo].id;
	if (extend && fAnchor >= 0) {
		fSelected.clear();
		int from = std::min(fAnchor, cell), to = std::max(fAnchor, cell);
		for (int i = from; i <= to; i++)
			fSelected.insert(fPhotos[fCells[i].photo].id);
	} else if (keep) {
		if (fSelected.count(id) != 0)
			fSelected.erase(id);
		else
			fSelected.insert(id);
		fAnchor = cell;
	} else {
		fSelected.clear();
		fSelected.insert(id);
		fAnchor = cell;
	}
	fFocus = cell;
	_EnsureVisible(cell);
	Invalidate();
	_SelectionChanged();
}


void
GridView::MouseDown(BPoint where)
{
	MakeFocus(true);
	BMessage* message = Window()->CurrentMessage();
	int32 buttons = message->GetInt32("buttons", B_PRIMARY_MOUSE_BUTTON);
	int32 clicks = message->GetInt32("clicks", 1);
	int32 modifiers = message->GetInt32("modifiers", 0);

	int header = _HeaderAt(where);
	if (header >= 0 && (buttons & B_PRIMARY_MOUSE_BUTTON) != 0
		&& fSummaries && (fMode == kGroupYears || fMode == kGroupMonths)) {
		// Into the year's months, the month's days.
		BMessage open(kMsgGroupMode);
		open.AddInt32("mode", fMode + 1);
		open.AddInt64("time", fSections[header].time);
		Window()->PostMessage(&open);
		return;
	}

	int cell = _CellAt(where);
	if ((buttons & B_SECONDARY_MOUSE_BUTTON) != 0
		|| ((buttons & B_PRIMARY_MOUSE_BUTTON) != 0 && (modifiers & B_CONTROL_KEY) != 0)) {
		if (cell >= 0 && fSelected.count(fPhotos[fCells[cell].photo].id) == 0)
			_SetFocusCell(cell, false, false);
		if (cell >= 0)
			_ShowContextMenu(where);
		return;
	}
	if (cell < 0) {
		if ((modifiers & (B_SHIFT_KEY | B_COMMAND_KEY)) == 0)
			ClearSelection();
		return;
	}
	if (clicks >= 2 && (modifiers & (B_SHIFT_KEY | B_COMMAND_KEY)) == 0) {
		BMessage open(kMsgOpenPhoto);
		open.AddInt64("id", fPhotos[fCells[cell].photo].id);
		Window()->PostMessage(&open);
		return;
	}
	_SetFocusCell(cell, (modifiers & B_SHIFT_KEY) != 0,
		(modifiers & B_COMMAND_KEY) != 0);
}


void
GridView::_ShowContextMenu(BPoint where)
{
	BPopUpMenu* menu = new BPopUpMenu("photo", false, false);
	int count = CountSelected();
	bool trash = false;
	bool remote = false;
	for (int64_t id : fSelected) {
		auto found = fIndex.find(id);
		if (found != fIndex.end()) {
			trash = trash || fPhotos[found->second].trashed;
			remote = remote || fPhotos[found->second].remoteOnly;
		}
	}
	if (trash) {
		menu->AddItem(new BMenuItem("Restore", new BMessage(kMsgRestore)));
		menu->AddItem(new BMenuItem("Delete for good…", new BMessage(kMsgDeleteForever)));
	} else {
		if (count == 1) {
			menu->AddItem(new BMenuItem("Open", new BMessage(kMsgOpenPhoto)));
			menu->AddItem(new BMenuItem("Edit", new BMessage(kMsgEdit)));
			menu->AddItem(new BMenuItem("Edit in PhotoCraft",
				new BMessage(kMsgEditInPhotoCraft)));
			menu->AddSeparatorItem();
		}
		menu->AddItem(new BMenuItem("Favorite", new BMessage(kMsgToggleFavorite)));
		menu->AddItem(new BMenuItem("Rotate left", new BMessage(kMsgRotateLeft)));
		menu->AddItem(new BMenuItem("Rotate right", new BMessage(kMsgRotateRight)));
		if (remote) {
			menu->AddItem(new BMenuItem("Download original",
				new BMessage(kMsgDownloadOriginal)));
		}
		menu->AddItem(new BMenuItem("Export…", new BMessage(kMsgExport)));
		menu->AddSeparatorItem();
		menu->AddItem(new BMenuItem(count > 1 ? "Move to Trash" : "Move to Trash",
			new BMessage(kMsgMoveToTrash)));
	}
	menu->SetTargetForItems(Window());
	ConvertToScreen(&where);
	menu->SetAsyncAutoDestruct(true);
	menu->Go(where, true, true, true);
}


void
GridView::KeyDown(const char* bytes, int32 numBytes)
{
	if (fCells.empty()) {
		BView::KeyDown(bytes, numBytes);
		return;
	}
	int32 modifiers = Window()->CurrentMessage()->GetInt32("modifiers", 0);
	bool extend = (modifiers & B_SHIFT_KEY) != 0;
	int cell = fFocus;
	switch (bytes[0]) {
		case B_LEFT_ARROW:
			_SetFocusCell(cell < 0 ? 0 : std::max(0, cell - 1), extend, false);
			return;
		case B_RIGHT_ARROW:
			_SetFocusCell(cell < 0 ? 0 : std::min((int)fCells.size() - 1, cell + 1),
				extend, false);
			return;
		case B_UP_ARROW:
		case B_DOWN_ARROW:
		{
			if (cell < 0) {
				_SetFocusCell(0, false, false);
				return;
			}
			// The tile above or below, across section headers.
			BRect rect = _CellRect(cell);
			float step = fTile + fGap;
			BPoint point(rect.left + fTile / 2, rect.top + fTile / 2);
			int target = -1;
			for (int tries = 0; tries < 8 && target < 0; tries++) {
				point.y += bytes[0] == B_UP_ARROW ? -step / 2 : step / 2;
				if (point.y < 0 || point.y > fTotalHeight)
					break;
				int hit = _CellAt(point);
				if (hit >= 0 && hit != cell)
					target = hit;
			}
			if (target < 0) {
				// A shorter last row: its last tile.
				int section = fCells[cell].section
					+ (bytes[0] == B_UP_ARROW ? -1 : 1);
				if (bytes[0] == B_DOWN_ARROW) {
					const Section& current = fSections[fCells[cell].section];
					int last = current.first + current.count - 1;
					if (cell / 1 != last && (cell - current.first) / fColumns
							< (current.count - 1) / fColumns)
						target = last;
				}
				if (target < 0 && section >= 0 && section < (int)fSections.size()) {
					const Section& next = fSections[section];
					target = bytes[0] == B_UP_ARROW
						? next.first + next.count - 1 : next.first;
				}
			}
			if (target >= 0)
				_SetFocusCell(target, extend, false);
			return;
		}
		case B_HOME:
			_SetFocusCell(0, extend, false);
			return;
		case B_END:
			_SetFocusCell((int)fCells.size() - 1, extend, false);
			return;
		case B_PAGE_UP:
		case B_PAGE_DOWN:
		{
			float maximum = std::max(0.0f, fTotalHeight - Bounds().Height());
			float y = Bounds().top + (bytes[0] == B_PAGE_UP ? -1 : 1)
				* (Bounds().Height() - fTile);
			BView::ScrollTo(BPoint(0, std::max(0.0f, std::min(y, maximum))));
			return;
		}
		case B_ENTER:
		case B_SPACE:
			if (cell >= 0) {
				BMessage open(kMsgOpenPhoto);
				open.AddInt64("id", fPhotos[fCells[cell].photo].id);
				Window()->PostMessage(&open);
			}
			return;
		case B_DELETE:
		case B_BACKSPACE:
			if (!fSelected.empty())
				Window()->PostMessage(kMsgMoveToTrash);
			return;
		case B_ESCAPE:
			ClearSelection();
			return;
	}
	BView::KeyDown(bytes, numBytes);
}


void
GridView::MessageReceived(BMessage* message)
{
	if (message->WasDropped()) {
		// Pictures dropped on the grid are imported: the window does it.
		Window()->PostMessage(message);
		return;
	}
	BView::MessageReceived(message);
}

}	// namespace ui
