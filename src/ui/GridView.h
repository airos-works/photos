// The grid of photos: every photo of a list as a square tile, in sections
// by year, month or day, or as one run. Only what is visible is drawn and
// loaded, so a library of any size scrolls the same.
#pragma once

#include "Ui.h"
#include "platform/Session.h"

#include <View.h>

#include <map>
#include <set>
#include <vector>

namespace ui {

class GridView : public BView {
public:
	explicit GridView(photos::Session& session);

	void SetPhotos(std::vector<photos::PhotoBrief>& photos, GroupMode mode,
		bool keepPosition);
	const std::vector<photos::PhotoBrief>& Photos() const { return fPhotos; }
	void SetGroupMode(GroupMode mode);
	GroupMode Mode() const { return fMode; }
	void SetTileSize(int size);
	int TileSize() const { return fTile; }
	void SetEmptyText(const char* title, const char* text);
	// Sections are summarised in the year and month modes; lists that are
	// not about time (a person's photos) show everything.
	void SetSummaries(bool summaries) { fSummaries = summaries; }

	std::vector<int64_t> Selection() const;
	int CountSelected() const { return (int)fSelected.size(); }
	bool IsSelected(int64_t id) const { return fSelected.count(id) != 0; }
	void SelectAll();
	void ClearSelection();
	void Select(int64_t id, bool scrollTo);
	int IndexOf(int64_t id) const;
	void ScrollToTime(int64_t taken);
	void TileReady(int64_t id);

	virtual void AttachedToWindow();
	virtual void Draw(BRect updateRect);
	virtual void FrameResized(float width, float height);
	virtual void MouseDown(BPoint where);
	virtual void KeyDown(const char* bytes, int32 numBytes);
	virtual void MessageReceived(BMessage* message);
	virtual void ScrollTo(BPoint where);
	virtual void MakeFocus(bool focus = true);

private:
	struct Section {
		BString title;
		BString subtitle;
		int64_t time = 0;		// of its first photo
		int first = 0;			// index of its first cell
		int count = 0;			// cells shown
		int total = 0;			// photos in it
		float top = 0;
		float tilesTop = 0;
		float bottom = 0;
	};
	struct Cell {
		int photo;				// index into fPhotos
		int section;
	};

	void _Build();
	void _Layout();
	void _UpdateScrollBar();
	int _Columns() const { return fColumns; }
	BRect _CellRect(int cell) const;
	int _CellAt(BPoint where) const;
	int _SectionAt(float y) const;
	int _HeaderAt(BPoint where) const;
	void _InvalidateCell(int cell);
	void _DrawCell(int cell, BRect rect);
	void _SetFocusCell(int cell, bool extend, bool keep);
	void _SelectionChanged();
	void _ShowContextMenu(BPoint where);
	void _EnsureVisible(int cell);

	photos::Session& fSession;
	std::vector<photos::PhotoBrief> fPhotos;
	std::map<int64_t, int> fIndex;			// photo id -> index in fPhotos
	std::vector<Section> fSections;
	std::vector<Cell> fCells;
	std::map<int, int> fCellOfPhoto;		// photo index -> cell
	std::set<int64_t> fSelected;
	GroupMode fMode;
	bool fSummaries;
	int fTile;
	int fColumns;
	float fGap;
	float fMargin;
	float fLeft;
	float fHeaderHeight;
	float fTotalHeight;
	int fFocus;				// cell with the keyboard focus, -1: none
	int fAnchor;			// where a shift-selection starts
	BString fEmptyTitle;
	BString fEmptyText;
};

}	// namespace ui
