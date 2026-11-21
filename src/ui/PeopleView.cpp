#include "PeopleView.h"

#include "Theme.h"

#include <Bitmap.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <StringItem.h>
#include <TextControl.h>

#include <algorithm>

using namespace photos;

namespace ui {

namespace {

const uint32 kMsgNameOk = 'nwOk';
const uint32 kMsgNameTyped = 'nwTy';
const uint32 kMsgNamePicked = 'nwPk';
const uint32 kMsgNameInvoked = 'nwIv';
const int kMaxGroups = 300;

}	// namespace


// --- PeopleView -------------------------------------------------------------

PeopleView::PeopleView(Session& session)
	:
	BView("people", B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
	fSession(session),
	fPeopleTop(0),
	fGroupsTop(0),
	fTotalHeight(0),
	fTile((int)S(128)),
	fHover(-1)
{
	SetViewColor(B_TRANSPARENT_COLOR);
	// A view without a layout would ask for the size it happens to have.
	SetExplicitMinSize(BSize(S(240), S(160)));
	SetExplicitPreferredSize(BSize(S(700), S(500)));
	SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
}


void
PeopleView::AttachedToWindow()
{
	BView::AttachedToWindow();
	_Layout();
}


void
PeopleView::Reload()
{
	fSession.Lib().ListPeople(fPeople);
	fPeopleCovers.clear();
	for (auto& person : fPeople) {
		int64_t cover = person.coverFace;
		Face face;
		if (cover == 0 || !fSession.Lib().GetFace(cover, face)
			|| face.personId != person.id)
			cover = fSession.Persons().BestFace(person.id);
		fPeopleCovers.push_back(cover);
	}
	// Groups of at least two faces: a face seen once is rarely worth naming
	// from here (it can be named on its photo).
	fSession.Persons().ListClusters(fClusters, 2);
	if ((int)fClusters.size() > kMaxGroups)
		fClusters.resize(kMaxGroups);
	_Layout();
	Invalidate();
}


void
PeopleView::_Layout()
{
	fTiles.clear();
	BRect bounds = Bounds();
	float margin = S(22), gap = S(18);
	font_height plain;
	be_plain_font->GetHeight(&plain);
	float labelHeight = (plain.ascent + plain.descent) * 2.6f;
	float cellHeight = fTile + labelHeight;
	int columns = std::max(1, (int)((bounds.Width() - 2 * margin + gap) / (fTile + gap)));
	float heading = S(44);
	float y = S(8);
	auto place = [&](int count, bool person) {
		for (int i = 0; i < count; i++) {
			int row = i / columns, column = i % columns;
			Tile tile;
			tile.rect = BRect(margin + column * (fTile + gap), y + row * (cellHeight + S(6)),
				0, 0);
			tile.rect.right = tile.rect.left + fTile - 1;
			tile.rect.bottom = tile.rect.top + cellHeight - 1;
			tile.person = person;
			tile.index = i;
			fTiles.push_back(tile);
		}
		int rows = (count + columns - 1) / columns;
		y += rows * (cellHeight + S(6));
	};
	fPeopleTop = y;
	y += heading;
	if (fPeople.empty())
		y += S(30);
	place((int)fPeople.size(), true);
	y += S(10);
	fGroupsTop = y;
	y += heading;
	if (fClusters.empty())
		y += S(30);
	place((int)fClusters.size(), false);
	fTotalHeight = y + S(20);
	_UpdateScrollBar();
}


void
PeopleView::_UpdateScrollBar()
{
	BScrollBar* bar = ScrollBar(B_VERTICAL);
	if (bar == nullptr)
		return;
	float height = Bounds().Height();
	float maximum = std::max(0.0f, fTotalHeight - height);
	bar->SetRange(0, maximum);
	bar->SetProportion(fTotalHeight > 0 ? std::min(1.0f, height / fTotalHeight) : 1);
	bar->SetSteps(S(40), std::max(S(40), height - S(60)));
	if (Bounds().top > maximum)
		ScrollTo(BPoint(0, maximum));
}


void
PeopleView::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	_Layout();
	Invalidate();
}


void
PeopleView::TileReady(int64_t faceId)
{
	for (auto& tile : fTiles) {
		int64_t face = tile.person ? fPeopleCovers[tile.index]
			: fClusters[tile.index].cover;
		if (face == faceId) {
			Invalidate(tile.rect);
			return;
		}
	}
}


int
PeopleView::_TileAt(BPoint where) const
{
	for (size_t i = 0; i < fTiles.size(); i++) {
		if (fTiles[i].rect.Contains(where))
			return (int)i;
	}
	return -1;
}


void
PeopleView::Draw(BRect updateRect)
{
	SetDrawingMode(B_OP_COPY);
	SetHighColor(Theme::Background());
	FillRect(updateRect);
	SetLowColor(Theme::Background());
	font_height plain;
	be_plain_font->GetHeight(&plain);
	float margin = S(22);

	auto heading = [&](float top, const char* title, const BString& note) {
		BFont font(be_bold_font);
		font.SetSize(be_plain_font->Size() * 1.35f);
		SetFont(&font);
		SetHighColor(Theme::Text());
		float baseline = top + S(30);
		DrawString(title, BPoint(margin, baseline));
		float x = margin + font.StringWidth(title) + S(12);
		SetFont(be_plain_font);
		SetHighColor(Theme::TertiaryText());
		DrawString(note.String(), BPoint(x, baseline));
	};
	FaceScanStatus scan = fSession.Faces().Status();
	BString peopleNote = fPeople.empty() ? BString("")
		: CountText((int)fPeople.size(), "person", "people");
	heading(fPeopleTop, "People", peopleNote);
	if (fPeople.empty()) {
		SetHighColor(Theme::SecondaryText());
		DrawString("Nobody has a name yet. Click a group of faces below and "
			"say who it is.", BPoint(margin, fPeopleTop + S(44) + plain.ascent));
	}
	BString groupsNote;
	if (scan.running) {
		groupsNote << "Looking at " << Number(scan.remaining) << " more "
			<< (scan.remaining == 1 ? "photo" : "photos") << "…";
	} else if (!fClusters.empty())
		groupsNote = "Click a face to name it";
	heading(fGroupsTop, "Unnamed faces", groupsNote);
	if (fClusters.empty()) {
		SetHighColor(Theme::SecondaryText());
		const char* text = !scan.error.empty() ? scan.error.c_str()
			: scan.running ? "Faces appear here as your photos are looked through."
			: "No unnamed faces. New photos are looked through as they arrive.";
		DrawString(text, BPoint(margin, fGroupsTop + S(44) + plain.ascent));
	}

	for (size_t i = 0; i < fTiles.size(); i++) {
		const Tile& tile = fTiles[i];
		if (!tile.rect.Intersects(updateRect))
			continue;
		int64_t face = tile.person ? fPeopleCovers[tile.index]
			: fClusters[tile.index].cover;
		BRect picture(tile.rect.left, tile.rect.top, tile.rect.left + fTile - 1,
			tile.rect.top + fTile - 1);
		std::shared_ptr<BBitmap> bitmap = face != 0
			? fSession.Pics().FaceTile(face, fTile) : nullptr;
		if (bitmap)
			DrawBitmap(bitmap.get(), picture.LeftTop());
		else {
			SetHighColor(Theme::Placeholder());
			FillRect(picture);
		}
		if ((int)i == fHover) {
			SetHighColor(Theme::Accent());
			SetPenSize(S(2));
			StrokeRect(picture.InsetByCopy(S(1), S(1)));
			SetPenSize(1);
		}
		BString name, note;
		if (tile.person) {
			name = fPeople[tile.index].name.c_str();
			note = CountText(fPeople[tile.index].photoCount, "photo", "photos");
		} else {
			name = "Add a name…";
			note = CountText(fClusters[tile.index].photoCount, "photo", "photos");
		}
		float baseline = picture.bottom + S(5) + plain.ascent;
		SetFont(tile.person ? be_bold_font : be_plain_font);
		SetHighColor(tile.person ? Theme::Text() : Theme::Accent());
		DrawTruncated(this, name.String(), BPoint(picture.left, baseline), fTile);
		BFont small(be_plain_font);
		small.SetSize(be_plain_font->Size() * 0.9f);
		SetFont(&small);
		SetHighColor(Theme::TertiaryText());
		DrawString(note.String(), BPoint(picture.left,
			baseline + plain.ascent + plain.descent + S(2)));
	}
}


void
PeopleView::MouseDown(BPoint where)
{
	int index = _TileAt(where);
	if (index < 0)
		return;
	const Tile& tile = fTiles[index];
	BMessage* current = Window()->CurrentMessage();
	int32 buttons = current->GetInt32("buttons", B_PRIMARY_MOUSE_BUTTON);
	if (tile.person) {
		const Person& person = fPeople[tile.index];
		if ((buttons & B_SECONDARY_MOUSE_BUTTON) != 0) {
			BPopUpMenu* menu = new BPopUpMenu("person", false, false);
			BMessage* rename = new BMessage(kMsgRenamePerson);
			rename->AddInt64("person", person.id);
			menu->AddItem(new BMenuItem("Rename or merge…", rename));
			BMessage* forget = new BMessage(kMsgDeletePerson);
			forget->AddInt64("person", person.id);
			menu->AddItem(new BMenuItem("Forget this person", forget));
			menu->SetTargetForItems(Window());
			ConvertToScreen(&where);
			menu->SetAsyncAutoDestruct(true);
			menu->Go(where, true, true, true);
			return;
		}
		BMessage open(kMsgOpenPerson);
		open.AddInt64("person", person.id);
		Window()->PostMessage(&open);
	} else {
		BMessage name(kMsgNameCluster);
		name.AddInt64("cluster", fClusters[tile.index].id);
		Window()->PostMessage(&name);
	}
}


void
PeopleView::MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
{
	int hover = transit == B_EXITED_VIEW ? -1 : _TileAt(where);
	if (hover != fHover) {
		if (fHover >= 0 && fHover < (int)fTiles.size())
			Invalidate(fTiles[fHover].rect);
		fHover = hover;
		if (fHover >= 0)
			Invalidate(fTiles[fHover].rect);
	}
}


// --- SuggestionsView --------------------------------------------------------

SuggestionsView::SuggestionsView(Session& session)
	:
	BView("suggestions", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
	fSession(session),
	fPersonId(0)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


BSize
SuggestionsView::MinSize()
{
	return BSize(S(200), S(150));
}


BSize
SuggestionsView::MaxSize()
{
	return BSize(B_SIZE_UNLIMITED, S(150));
}


BSize
SuggestionsView::PreferredSize()
{
	return BSize(S(800), S(150));
}


void
SuggestionsView::SetPerson(int64_t personId)
{
	fPersonId = personId;
	fSuggestions.clear();
	fName = "";
	if (personId != 0) {
		Person person;
		if (fSession.Lib().GetPerson(personId, person))
			fName = person.name.c_str();
		fSession.Persons().Suggestions(personId, fSuggestions, 40);
	}
	bool show = !fSuggestions.empty();
	if (show && IsHidden())
		Show();
	else if (!show && !IsHidden())
		Hide();
	Invalidate();
}


void
SuggestionsView::TileReady(int64_t faceId)
{
	for (auto& suggestion : fSuggestions) {
		if (suggestion.faceId == faceId) {
			Invalidate();
			return;
		}
	}
}


void
SuggestionsView::Draw(BRect updateRect)
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
	BString title;
	title << "Is this " << fName << " too?";
	SetFont(be_bold_font);
	SetHighColor(Theme::Text());
	DrawString(title.String(), BPoint(S(18), S(10) + plain.ascent));

	float tile = S(84);
	float buttonHeight = ceilf((plain.ascent + plain.descent) * 1.5f);
	float x = S(18);
	float top = S(14) + plain.ascent + plain.descent;
	fYes.assign(fSuggestions.size(), BRect());
	fNo.assign(fSuggestions.size(), BRect());
	fFaces.assign(fSuggestions.size(), BRect());
	for (size_t i = 0; i < fSuggestions.size(); i++) {
		if (x + tile > bounds.right - S(10))
			break;
		BRect picture(x, top, x + tile - 1, top + tile - 1);
		fFaces[i] = picture;
		std::shared_ptr<BBitmap> bitmap = fSession.Pics().FaceTile(
			fSuggestions[i].faceId, (int)tile);
		if (bitmap)
			DrawBitmap(bitmap.get(), picture.LeftTop());
		else {
			SetHighColor(Theme::Placeholder());
			FillRect(picture);
		}
		float half = floorf((tile - S(4)) / 2);
		BRect yes(x, picture.bottom + S(4), x + half, picture.bottom + S(4) + buttonHeight);
		BRect no(x + half + S(4), yes.top, x + tile - 1, yes.bottom);
		fYes[i] = yes;
		fNo[i] = no;
		SetHighColor(Theme::Accent());
		FillRoundRect(yes, S(5), S(5));
		SetHighColor(Theme::Control());
		FillRoundRect(no, S(5), S(5));
		SetFont(be_plain_font);
		SetLowColor(Theme::Accent());
		SetHighColor(Theme::AccentText());
		DrawString("Yes", BPoint(yes.left + (yes.Width()
			- be_plain_font->StringWidth("Yes")) / 2,
			yes.top + (yes.Height() + plain.ascent - plain.descent) / 2));
		SetLowColor(Theme::Control());
		SetHighColor(Theme::Text());
		DrawString("No", BPoint(no.left + (no.Width()
			- be_plain_font->StringWidth("No")) / 2,
			no.top + (no.Height() + plain.ascent - plain.descent) / 2));
		SetLowColor(Theme::Header());
		x += tile + S(12);
	}
}


void
SuggestionsView::MouseDown(BPoint where)
{
	for (size_t i = 0; i < fSuggestions.size(); i++) {
		uint32 what = 0;
		if (i < fYes.size() && fYes[i].IsValid() && fYes[i].Contains(where))
			what = kMsgConfirmFace;
		else if (i < fNo.size() && fNo[i].IsValid() && fNo[i].Contains(where))
			what = kMsgRejectFace;
		else if (i < fFaces.size() && fFaces[i].IsValid() && fFaces[i].Contains(where)) {
			// The photo the face is from.
			BMessage open(kMsgOpenPhoto);
			open.AddInt64("id", fSuggestions[i].photoId);
			Window()->PostMessage(&open);
			return;
		}
		if (what == 0)
			continue;
		BMessage message(what);
		message.AddInt64("face", fSuggestions[i].faceId);
		message.AddInt64("person", fPersonId);
		Window()->PostMessage(&message);
		// Gone from the row at once; the window reloads when it is done.
		fSuggestions.erase(fSuggestions.begin() + i);
		Invalidate();
		return;
	}
}


// --- NameWindow -------------------------------------------------------------

NameWindow::NameWindow(BWindow* parent, Session& session, const char* title,
	const char* initial, const BMessage& reply)
	:
	BWindow(BRect(0, 0, S(320), S(340)), title, B_FLOATING_WINDOW_LOOK,
		B_MODAL_SUBSET_WINDOW_FEEL, B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE
			| B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
	fSession(session),
	fTarget(parent),
	fReply(reply)
{
	AddToSubset(parent);
	fName = new BTextControl("name", "Name:", initial, new BMessage(kMsgNameOk));
	fName->SetModificationMessage(new BMessage(kMsgNameTyped));
	fList = new BListView("people");
	fList->SetSelectionMessage(new BMessage(kMsgNamePicked));
	fList->SetInvocationMessage(new BMessage(kMsgNameInvoked));
	BScrollView* scroll = new BScrollView("scroll", fList, 0, false, true);
	scroll->SetExplicitMinSize(BSize(S(280), S(180)));
	BButton* cancel = new BButton("cancel", "Cancel", new BMessage(B_QUIT_REQUESTED));
	BButton* ok = new BButton("ok", "OK", new BMessage(kMsgNameOk));
	BLayoutBuilder::Group<>(this, B_VERTICAL, S(8))
		.SetInsets(S(12))
		.Add(fName)
		.Add(scroll)
		.AddGroup(B_HORIZONTAL, S(8))
			.AddGlue()
			.Add(cancel)
			.Add(ok)
		.End();
	ok->MakeDefault(true);
	fSession.Lib().ListPeople(fPeople, true);
	_Filter();
	fName->MakeFocus(true);
	CenterIn(parent->Frame());
}


void
NameWindow::_Filter()
{
	BString typed(fName->Text());
	typed.Trim();
	fList->MakeEmpty();
	for (auto& person : fPeople) {
		BString name(person.name.c_str());
		if (typed.Length() == 0 || name.IFindFirst(typed) >= 0)
			fList->AddItem(new BStringItem(name.String()));
	}
}


void
NameWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgNameTyped:
			_Filter();
			break;

		case kMsgNamePicked:
		case kMsgNameInvoked:
		{
			BStringItem* item = dynamic_cast<BStringItem*>(
				fList->ItemAt(fList->CurrentSelection()));
			if (item == nullptr)
				break;
			BString name(item->Text());
			if (message->what == kMsgNamePicked) {
				fName->SetModificationMessage(nullptr);
				fName->SetText(name.String());
				fName->SetModificationMessage(new BMessage(kMsgNameTyped));
				break;
			}
			fName->SetText(name.String());
			PostMessage(kMsgNameOk);
			break;
		}

		case kMsgNameOk:
		{
			BString name(fName->Text());
			name.Trim();
			if (name.Length() == 0)
				break;
			BMessage reply(fReply);
			reply.AddString("name", name);
			fTarget.SendMessage(&reply);
			PostMessage(B_QUIT_REQUESTED);
			break;
		}

		default:
			BWindow::MessageReceived(message);
	}
}

}	// namespace ui
