// The window's frame around the photos: the sidebar, the header with its
// buttons, the status line.
#pragma once

#include "Ui.h"

#include <View.h>

#include <vector>

namespace ui {

// --- Sidebar ----------------------------------------------------------------

struct SidebarRow {
	bool heading = false;
	int item = 0;				// SidebarItem
	int64_t argument = 0;
	BString label;
	BString count;
	BString key;				// cameras: the device
};

class SidebarView : public BView {
public:
	SidebarView();

	void SetRows(const std::vector<SidebarRow>& rows);
	void Select(int item, int64_t argument);
	int SelectedItem() const { return fItem; }
	int64_t SelectedArgument() const { return fArgument; }
	// The line at the bottom about Immich.
	void SetSync(const char* title, const char* text, bool busy, bool error);

	virtual void Draw(BRect updateRect);
	virtual void MouseDown(BPoint where);
	virtual void MouseMoved(BPoint where, uint32 transit, const BMessage* drag);
	virtual BSize MinSize();
	virtual BSize MaxSize();
	virtual BSize PreferredSize();

private:
	int _RowAt(BPoint where) const;
	BRect _SyncRect() const;

	std::vector<SidebarRow> fRows;
	std::vector<BRect> fRects;
	int fItem;
	int64_t fArgument;
	int fHover;
	BString fSyncTitle;
	BString fSyncText;
	bool fSyncBusy;
	bool fSyncError;
	bool fSyncHover;
};

// --- Header -----------------------------------------------------------------

enum {
	kButtonAccent = 1,		// the main action
	kButtonDanger = 2,
	kButtonOn = 4,			// a toggle that is on
	kButtonDisabled = 8,
	kButtonHeart = 16,		// drawn as a heart (filled when on)
	kButtonLeftSide = 32	// next to the title instead of at the right
};

class HeaderView : public BView {
public:
	HeaderView();

	void Clear();
	void SetBack(const char* label);			// null: no way back
	void SetTitle(const char* title, const char* subtitle);
	void SetSegments(bool show, int selected);	// Years / Months / Days / All
	void SetZoom(bool show, int tileSize);
	void AddButton(const char* label, uint32 what, uint32 flags = 0);
	void Finish();								// after the calls above

	virtual void Draw(BRect updateRect);
	virtual void MouseDown(BPoint where);
	virtual void MouseMoved(BPoint where, uint32 transit, const BMessage* drag);
	virtual void MouseUp(BPoint where);
	virtual BSize MinSize();
	virtual BSize MaxSize();
	virtual BSize PreferredSize();

private:
	struct Button {
		BString label;
		uint32 what;
		uint32 flags;
		BRect rect;
	};

	void _Layout();
	void _SetZoomFrom(float x);

	BString fBack;
	BString fTitle;
	BString fSubtitle;
	bool fShowSegments;
	int fSegment;
	bool fShowZoom;
	int fTileSize;
	std::vector<Button> fButtons;
	BRect fBackRect;
	BRect fSegmentRects[4];
	BRect fZoomRect;
	int fHover;				// index of the button under the mouse, -2: back
	bool fZoomDrag;
};

// --- Status line ------------------------------------------------------------

class StatusView : public BView {
public:
	StatusView();

	void SetLeft(const char* text);
	void SetRight(const char* text, float progress = -1);	// progress 0..1, <0: none

	virtual void Draw(BRect updateRect);
	virtual BSize MinSize();
	virtual BSize MaxSize();
	virtual BSize PreferredSize();

private:
	BString fLeft;
	BString fRight;
	float fProgress;
};

}	// namespace ui
