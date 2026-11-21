#include "MainWindow.h"

#include "ImportView.h"
#include "SettingsWindow.h"
#include "Theme.h"
#include "platform/Editing.h"
#include "platform/Messages.h"

#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <CardLayout.h>
#include <Entry.h>
#include <FilePanel.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Path.h>
#include <Roster.h>
#include <Screen.h>
#include <ScrollView.h>

#include <algorithm>

using namespace photos;

namespace ui {

namespace {

const uint32 kMsgTick = 'tick';
const uint32 kMsgBusyDone = 'bsyD';		// "error" (string) when it failed
const uint32 kMsgAbout = 'abou';
const uint32 kMsgSelectNone = 'selN';
const uint32 kMsgZoomFit = 'zmFt';
const uint32 kMsgZoomActual = 'zmAc';

// A number from a message, whichever integer type it was sent as (hey sends
// int32).
int64_t
Id(const BMessage* message, const char* name)
{
	int64 value;
	if (message->FindInt64(name, &value) == B_OK)
		return value;
	int32 small;
	if (message->FindInt32(name, &small) == B_OK)
		return small;
	return 0;
}

const char* const kGroupModeSetting = "ui.group_mode";
const char* const kInfoSetting = "ui.info";
const char* const kFrameSetting = "ui.frame";

}	// namespace


MainWindow::MainWindow(Session& session)
	:
	BWindow(BRect(0, 0, S(1180), S(760)), "Photos", B_DOCUMENT_WINDOW,
		B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS),
	fSession(session),
	fImportView(nullptr),
	fOpenPanel(nullptr),
	fExportPanel(nullptr),
	fTicker(nullptr),
	fSettingsWindow(nullptr),
	fItem(-1),
	fArgument(0),
	fGroupMode(kGroupDays),
	fCard(kCardGrid),
	fCardBeforeViewer(kCardGrid),
	fViewerOpen(false),
	fSidePanelShown(0),
	fInfoWanted(false),
	fEditSaving(false),
	fReloadPending(false),
	fLastSidebar(0),
	fBusyCount(0),
	fImportProgress(-1)
{
	Theme::Refresh();
	Settings& prefs = fSession.Prefs();
	fGroupMode = (int)prefs.GetInt(kGroupModeSetting, kGroupDays);
	if (fGroupMode < kGroupYears || fGroupMode > kGroupNone)
		fGroupMode = kGroupDays;
	fInfoWanted = prefs.GetBool(kInfoSetting, false);

	fSidebar = new SidebarView();
	fHeader = new HeaderView();
	fStatus = new StatusView();
	fSuggestions = new SuggestionsView(fSession);
	fGrid = new GridView(fSession);
	fGrid->SetTileSize((int)prefs.GetInt(setting::kTileSize, (int)S(150)));
	fViewer = new ViewerView(fSession);
	fInfo = new InfoView(fSession);
	fEditPanel = new EditPanel();
	fPeopleView = new PeopleView(fSession);
	fImportView = new ImportView(fSession);
	fEdit = new EditSession(fSession, BMessenger(this));

	BScrollView* gridScroll = new BScrollView("grid scroll", fGrid, 0, false, true,
		B_NO_BORDER);
	BScrollView* peopleScroll = new BScrollView("people scroll", fPeopleView, 0,
		false, true, B_NO_BORDER);

	fSidePanel = new BView("side panel", 0);
	fSideCards = new BCardLayout();
	fSidePanel->SetLayout(fSideCards);
	fSideCards->AddView(fInfo);
	// The editor's controls scroll when the window is not tall enough.
	fSideCards->AddView(new ScrollBox("edit box", fEditPanel, S(286)));
	fSidePanel->SetExplicitMinSize(BSize(S(286), S(120)));
	fSidePanel->SetExplicitMaxSize(BSize(S(286), B_SIZE_UNLIMITED));
	fSidePanel->Hide();

	BGroupView* viewerCard = new BGroupView(B_HORIZONTAL, 0);
	viewerCard->GroupLayout()->AddView(fViewer);
	viewerCard->GroupLayout()->AddView(fSidePanel);

	BView* cards = new BView("cards", 0);
	fCards = new BCardLayout();
	cards->SetLayout(fCards);
	fCards->AddView(gridScroll);
	fCards->AddView(viewerCard);
	fCards->AddView(peopleScroll);
	fCards->AddView(fImportView);
	fCards->SetVisibleItem((int32)0);

	BMenuBar* menuBar = new BMenuBar("menu");
	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(menuBar)
		.AddGroup(B_HORIZONTAL, 0)
			.Add(fSidebar)
			.AddGroup(B_VERTICAL, 0)
				.Add(fHeader)
				.Add(fSuggestions)
				.Add(cards)
				.Add(fStatus)
			.End()
		.End();
	fSuggestions->Hide();
	_BuildMenus();

	// Where the window was, if that is still on a screen.
	BRect frame;
	BString saved = prefs.GetString(kFrameSetting).c_str();
	float left, top, right, bottom;
	if (sscanf(saved.String(), "%f,%f,%f,%f", &left, &top, &right, &bottom) == 4) {
		frame.Set(left, top, right, bottom);
		BRect screen = BScreen(this).Frame();
		if (frame.IsValid() && screen.Contains(frame.LeftTop() + BPoint(40, 40))
			&& frame.Width() > S(500) && frame.Height() > S(300)) {
			MoveTo(frame.LeftTop());
			ResizeTo(frame.Width(), frame.Height());
		} else
			CenterOnScreen();
	} else {
		BRect screen = BScreen(this).Frame();
		ResizeTo(std::min(S(1280), screen.Width() * 0.86f),
			std::min(S(820), screen.Height() * 0.84f));
		CenterOnScreen();
	}

#ifdef PHOTOS_HAVE_CAMERAS
	fCameras = new CameraWatcher(BMessenger(this));
#endif
	BMessage tick(kMsgTick);
	fTicker = new BMessageRunner(BMessenger(this), &tick, 1500000);

	_ShowView(kItemPhotos, 0, true);
	_RebuildSidebar();
	fGrid->MakeFocus(true);
#ifdef PHOTOS_HAVE_CAMERAS
	fCameras->Start();
#endif
}


MainWindow::~MainWindow()
{
	delete fTicker;
	delete fEdit;
	delete fOpenPanel;
	delete fExportPanel;
#ifdef PHOTOS_HAVE_CAMERAS
	delete fCameras;
#endif
}


void
MainWindow::_BuildMenus()
{
	BMenuBar* bar = KeyMenuBar();
	BMenu* file = new BMenu("File");
	file->AddItem(new BMenuItem("Import…", new BMessage(kMsgImportFiles), 'I'));
	file->AddItem(new BMenuItem("Export…", new BMessage(kMsgExport), 'E', B_SHIFT_KEY));
	file->AddSeparatorItem();
	file->AddItem(new BMenuItem("Sync with Immich now", new BMessage(kMsgSyncNow), 'R'));
	file->AddItem(new BMenuItem("Settings…", new BMessage(kMsgSettings), ','));
	file->AddSeparatorItem();
	file->AddItem(new BMenuItem("About Photos", new BMessage(kMsgAbout)));
	file->AddItem(new BMenuItem("Quit", new BMessage(B_QUIT_REQUESTED), 'Q'));
	bar->AddItem(file);

	BMenu* edit = new BMenu("Edit");
	edit->AddItem(new BMenuItem("Select all", new BMessage(kMsgSelectAll), 'A'));
	edit->AddItem(new BMenuItem("Select none", new BMessage(kMsgSelectNone), 'A',
		B_SHIFT_KEY));
	edit->AddSeparatorItem();
	edit->AddItem(new BMenuItem("Move to Trash", new BMessage(kMsgMoveToTrash), 'T'));
	edit->AddItem(new BMenuItem("Restore from Trash", new BMessage(kMsgRestore)));
	edit->AddItem(new BMenuItem("Empty Trash…", new BMessage(kMsgEmptyTrash)));
	bar->AddItem(edit);

	BMenu* photo = new BMenu("Photo");
	photo->AddItem(new BMenuItem("Open", new BMessage(kMsgOpenPhoto), 'O'));
	photo->AddItem(new BMenuItem("Edit", new BMessage(kMsgEdit), 'E'));
	photo->AddItem(new BMenuItem("Edit in PhotoCraft", new BMessage(kMsgEditInPhotoCraft),
		'P'));
	photo->AddItem(new BMenuItem("Open the original in LightCraft",
		new BMessage(kMsgOpenInLightCraft)));
	photo->AddSeparatorItem();
	photo->AddItem(new BMenuItem("Favorite", new BMessage(kMsgToggleFavorite), '.'));
	photo->AddItem(new BMenuItem("Rotate left", new BMessage(kMsgRotateLeft), 'L'));
	photo->AddItem(new BMenuItem("Rotate right", new BMessage(kMsgRotateRight), 'R',
		B_SHIFT_KEY));
	photo->AddItem(new BMenuItem("Revert to original", new BMessage(kMsgRevert)));
	photo->AddSeparatorItem();
	photo->AddItem(new BMenuItem("Download original from Immich",
		new BMessage(kMsgDownloadOriginal)));
	photo->AddItem(new BMenuItem("Show original in Tracker",
		new BMessage(kMsgShowInTracker)));
	bar->AddItem(photo);

	BMenu* view = new BMenu("View");
	static const char* const kModes[] = {"Years", "Months", "Days", "All Photos"};
	for (int i = 0; i < 4; i++) {
		BMessage* message = new BMessage(kMsgGroupMode);
		message->AddInt32("mode", i);
		view->AddItem(new BMenuItem(kModes[i], message, '1' + i));
	}
	view->AddSeparatorItem();
	view->AddItem(new BMenuItem("Larger", new BMessage(kMsgZoomIn), '+'));
	view->AddItem(new BMenuItem("Smaller", new BMessage(kMsgZoomOut), '-'));
	view->AddItem(new BMenuItem("Fit to window", new BMessage(kMsgZoomFit), '0'));
	view->AddItem(new BMenuItem("Actual size", new BMessage(kMsgZoomActual)));
	view->AddSeparatorItem();
	view->AddItem(new BMenuItem("Info", new BMessage(kMsgToggleInfo), 'I', B_SHIFT_KEY));
	view->AddItem(new BMenuItem("Faces", new BMessage(kMsgToggleFaces), 'F', B_SHIFT_KEY));
	bar->AddItem(view);
}


void
MainWindow::MenusBeginning()
{
	BMenuBar* bar = KeyMenuBar();
	std::vector<int64_t> targets = _Targets();
	bool any = !targets.empty();
	bool one = targets.size() == 1;
	bool trash = fItem == kItemTrash;
	auto enable = [&](uint32 what, bool enabled) {
		BMenuItem* item = bar->FindItem(what);
		if (item != nullptr)
			item->SetEnabled(enabled);
	};
	bool editing = fEdit->IsActive();
	enable(kMsgExport, any && !trash && !editing);
	enable(kMsgMoveToTrash, any && !trash && !editing);
	enable(kMsgRestore, any && trash);
	enable(kMsgEmptyTrash, fSession.Lib().Count(kFilterTrash) > 0);
	enable(kMsgOpenPhoto, one && !fViewerOpen);
	enable(kMsgEdit, one && !trash && !editing);
	enable(kMsgEditInPhotoCraft, one && !trash && !editing);
	enable(kMsgToggleFavorite, any && !trash);
	enable(kMsgRotateLeft, any && !trash && !editing);
	enable(kMsgRotateRight, any && !trash && !editing);
	enable(kMsgSyncNow, fSession.Sync().IsConfigured());
	Photo photo;
	bool have = one && fSession.Lib().Get(targets[0], photo);
	enable(kMsgRevert, have && photo.version != 0 && !editing);
	enable(kMsgDownloadOriginal, have && photo.path.empty());
	enable(kMsgShowInTracker, have && !photo.path.empty());
	enable(kMsgOpenInLightCraft, have && photo.kind == kKindRaw && !editing);
	BMenuItem* favorite = bar->FindItem(kMsgToggleFavorite);
	if (favorite != nullptr)
		favorite->SetLabel(have && photo.favorite ? "Remove from Favorites" : "Favorite");
	enable(kMsgToggleInfo, fViewerOpen && !editing);
	enable(kMsgToggleFaces, fViewerOpen && !editing);
	enable(kMsgZoomFit, fViewerOpen);
	enable(kMsgZoomActual, fViewerOpen);
	bool timeline = fItem == kItemPhotos && !fViewerOpen;
	for (int i = 0; i < 4; i++) {
		BMenu* view = bar->SubmenuAt(3);
		if (view != nullptr && view->ItemAt(i) != nullptr) {
			view->ItemAt(i)->SetEnabled(timeline);
			view->ItemAt(i)->SetMarked(timeline && fGroupMode == i);
		}
	}
}


void
MainWindow::_Alert(const char* title, const char* text)
{
	BAlert* alert = new BAlert(title, text, "OK", nullptr, nullptr,
		B_WIDTH_AS_USUAL, B_WARNING_ALERT);
	alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
	alert->Go(nullptr);
}


void
MainWindow::_ShowCard(Card card)
{
	fCard = card;
	fCards->SetVisibleItem((int32)card);
}


void
MainWindow::_SetSidePanel(int panel)
{
	fSidePanelShown = panel;
	if (panel == 0) {
		if (!fSidePanel->IsHidden())
			fSidePanel->Hide();
		return;
	}
	fSideCards->SetVisibleItem((int32)(panel - 1));
	if (fSidePanel->IsHidden())
		fSidePanel->Show();
}


// --- Views of the library ---------------------------------------------------

void
MainWindow::_ShowView(int item, int64_t argument, bool force)
{
	if (fEdit->IsActive())
		_EndEdit(false);
	if (fViewerOpen) {
		fViewerOpen = false;
		fViewer->Clear();
		_SetSidePanel(0);
	}
	if (item == kItemLastImport) {
		std::vector<Import> imports;
		fSession.Lib().ListImports(imports);
		argument = imports.empty() ? 0 : imports[0].id;
	}
	bool same = item == fItem && argument == fArgument && !force;
	fItem = item;
	fArgument = argument;
	fSidebar->Select(item, argument);
	if (item == kItemPeople) {
		fSuggestions->SetPerson(0);
		fPeopleView->Reload();
		_ShowCard(kCardPeople);
	} else if (item == kItemCamera) {
		fSuggestions->SetPerson(0);
		_ShowCard(kCardImport);
	} else {
		fSuggestions->SetPerson(item == kItemPerson ? argument : 0);
		_ReloadList(same);
		_ShowCard(kCardGrid);
		fGrid->MakeFocus(true);
	}
	_UpdateHeader();
	_UpdateStatus();
}


void
MainWindow::_ReloadList(bool keepPosition)
{
	std::vector<PhotoBrief> photos;
	GroupMode mode = kGroupNone;
	bool summaries = false;
	const char* emptyTitle = "No photos";
	const char* emptyText = "";
	switch (fItem) {
		case kItemPhotos:
			fSession.Lib().List(kFilterAll, 0, photos);
			mode = (GroupMode)fGroupMode;
			summaries = true;
			emptyTitle = "No photos yet";
			emptyText = "Drop pictures or folders here, or choose File › Import.\n"
				"Connect a camera, a phone or a memory card to import from it.\n"
				"File › Settings connects Photos to your Immich server.";
			break;
		case kItemRecents:
			fSession.Lib().List(kFilterRecent, 0, photos);
			emptyTitle = "Nothing recent";
			emptyText = "The photos added last appear here.";
			break;
		case kItemFavorites:
			fSession.Lib().List(kFilterFavorites, 0, photos);
			mode = kGroupMonths;
			emptyTitle = "No favorites";
			emptyText = "Click the heart on a photo to keep it here.";
			break;
		case kItemEdited:
			fSession.Lib().List(kFilterEdited, 0, photos);
			mode = kGroupMonths;
			emptyTitle = "No edited photos";
			emptyText = "Photos you edit here or in PhotoCraft appear here.";
			break;
		case kItemLastImport:
		case kItemImport:
			if (fArgument != 0)
				fSession.Lib().List(kFilterImport, fArgument, photos);
			emptyTitle = "Nothing imported";
			emptyText = "The photos of your last import appear here.";
			break;
		case kItemTrash:
			fSession.Lib().List(kFilterTrash, 0, photos);
			emptyTitle = "The trash is empty";
			emptyText = "Deleted photos stay here for 30 days.";
			break;
		case kItemPerson:
			fSession.Lib().List(kFilterPerson, fArgument, photos);
			mode = kGroupMonths;
			emptyTitle = "No photos of this person";
			break;
	}
	fGrid->SetSummaries(summaries);
	fGrid->SetEmptyText(emptyTitle, emptyText);
	fGrid->SetPhotos(photos, mode, keepPosition);
}


void
MainWindow::_RebuildSidebar()
{
	fLastSidebar = system_time();
	Library& library = fSession.Lib();
	std::vector<SidebarRow> rows;
	auto heading = [&](const char* label) {
		SidebarRow row;
		row.heading = true;
		row.label = label;
		rows.push_back(row);
	};
	auto item = [&](int which, int64_t argument, const char* label, int count) {
		SidebarRow row;
		row.item = which;
		row.argument = argument;
		row.label = label;
		if (count > 0)
			row.count = Number(count);
		rows.push_back(row);
	};
	heading("Library");
	item(kItemPhotos, 0, "Photos", library.Count(kFilterAll));
	item(kItemRecents, 0, "Recents", 0);
	item(kItemFavorites, 0, "Favorites", library.Count(kFilterFavorites));
	item(kItemPeople, 0, "People", 0);
	int edited = library.Count(kFilterEdited);
	if (edited > 0 || fItem == kItemEdited)
		item(kItemEdited, 0, "Edited", edited);
	std::vector<Import> imports;
	library.ListImports(imports);
	if (!imports.empty()) {
		item(kItemLastImport, fItem == kItemLastImport ? fArgument : imports[0].id,
			"Last Import", imports[0].count);
	}
	int trash = library.Count(kFilterTrash);
	if (trash > 0 || fItem == kItemTrash)
		item(kItemTrash, 0, "Recently Deleted", trash);

	std::vector<Person> people;
	library.ListPeople(people);
	if (!people.empty()) {
		heading("People");
		for (size_t i = 0; i < people.size() && i < 10; i++) {
			item(kItemPerson, people[i].id, people[i].name.c_str(),
				people[i].photoCount);
		}
	}
	if (fItem == kItemPerson) {
		// A person further down the list is still shown while it is open.
		bool listed = false;
		for (size_t i = 0; i < people.size() && i < 10; i++)
			listed = listed || people[i].id == fArgument;
		Person person;
		if (!listed && library.GetPerson(fArgument, person)) {
			if (people.empty())
				heading("People");
			item(kItemPerson, person.id, person.name.c_str(), person.photoCount);
		}
	}

	if (!fCameraKeys.empty()) {
		heading("Devices");
		for (size_t i = 0; i < fCameraKeys.size(); i++) {
			SidebarRow row;
			row.item = kItemCamera;
			row.argument = (int64_t)i;
			row.label = fCameraNames[i];
			row.key = fCameraKeys[i];
			rows.push_back(row);
		}
	}
	fSidebar->SetRows(rows);

	SyncState sync = fSession.GetSyncState();
	if (!sync.configured)
		fSidebar->SetSync("Immich", "Not connected · click to set up", false, false);
	else if (!sync.error.empty() && !sync.running)
		fSidebar->SetSync("Immich", sync.error.c_str(), false, true);
	else {
		fSidebar->SetSync("Immich", sync.text.empty() ? "Click to sync"
			: sync.text.c_str(), sync.running, false);
	}
}


void
MainWindow::_UpdateHeader()
{
	fHeader->Clear();
	std::vector<int64_t> selection = fGrid->Selection();
	if (fEdit->IsActive()) {
		Photo photo;
		fSession.Lib().Get(fEdit->PhotoId(), photo);
		fHeader->SetTitle("Edit", photo.name.c_str());
		fHeader->AddButton("Cancel", kMsgEditCancel);
		fHeader->AddButton(fEditSaving ? "Saving…" : "Done", kMsgEditDone,
			kButtonAccent | (fEditSaving || !fEdit->IsReady() ? kButtonDisabled : 0));
		fHeader->Finish();
		return;
	}
	if (fViewerOpen) {
		Photo photo;
		fSession.Lib().Get(fViewer->PhotoId(), photo);
		const char* back = fItem == kItemPhotos ? "Photos" : fItem == kItemPeople
			? "People" : "Back";
		fHeader->SetBack(back);
		fHeader->SetTitle(photo.taken != 0 ? DateTimeText(photo.taken).String()
			: photo.name.c_str(), photo.taken != 0 ? photo.name.c_str() : "");
		if (photo.trashed != 0) {
			fHeader->AddButton("Restore", kMsgRestore);
			fHeader->AddButton("Delete for good", kMsgDeleteForever, kButtonDanger);
		} else {
			fHeader->AddButton("‹", kMsgPrevious);
			fHeader->AddButton("›", kMsgNext);
			fHeader->AddButton("", kMsgToggleFavorite,
				kButtonHeart | (photo.favorite ? kButtonOn : 0));
			fHeader->AddButton("Rotate", kMsgRotateRight);
			fHeader->AddButton("Edit", kMsgEdit, kButtonAccent);
			if (PhotoCraftBridge::IsInstalled(kPhotoCraftSignature))
				fHeader->AddButton("PhotoCraft", kMsgEditInPhotoCraft);
			fHeader->AddButton("Faces", kMsgToggleFaces,
				fViewer->ShowsFaces() ? kButtonOn : 0);
			fHeader->AddButton("Info", kMsgToggleInfo,
				fSidePanelShown == 1 ? kButtonOn : 0);
			fHeader->AddButton("Delete", kMsgMoveToTrash, kButtonDanger);
		}
		fHeader->Finish();
		return;
	}

	BString title, subtitle;
	bool segments = false;
	bool zoom = true;
	switch (fItem) {
		case kItemPhotos:
			title = "Photos";
			segments = true;
			break;
		case kItemRecents:
			title = "Recents";
			subtitle = "The photos added last";
			break;
		case kItemFavorites:
			title = "Favorites";
			break;
		case kItemEdited:
			title = "Edited";
			break;
		case kItemLastImport:
		case kItemImport:
		{
			title = "Last Import";
			std::vector<Import> imports;
			fSession.Lib().ListImports(imports);
			for (auto& import : imports) {
				if (import.id == fArgument) {
					int64_t local = import.started + LocalOffsetSeconds(import.started);
					subtitle << import.source.c_str() << "  ·  " << DateTimeText(local);
				}
			}
			break;
		}
		case kItemTrash:
			title = "Recently Deleted";
			subtitle = "Photos stay here for 30 days, then they are deleted for good";
			break;
		case kItemPerson:
		{
			Person person;
			if (fSession.Lib().GetPerson(fArgument, person)) {
				title = person.name.c_str();
				subtitle = CountText(person.photoCount, "photo", "photos");
			}
			break;
		}
		case kItemPeople:
			title = "People";
			zoom = false;
			break;
		case kItemCamera:
			title = fImportView->Title();
			subtitle = fImportView->Subtitle();
			zoom = false;
			break;
	}
	fHeader->SetTitle(title.String(), subtitle.String());
	fHeader->SetSegments(segments, fGroupMode);
	fHeader->SetZoom(zoom, fGrid->TileSize());
	if (fItem == kItemPerson)
		fHeader->SetBack("People");
	if (fItem == kItemTrash) {
		if (!selection.empty()) {
			fHeader->AddButton("Restore", kMsgRestore);
			fHeader->AddButton("Delete for good", kMsgDeleteForever, kButtonDanger);
		} else if (!fGrid->Photos().empty())
			fHeader->AddButton("Empty Trash", kMsgEmptyTrash, kButtonDanger);
	} else if (fItem == kItemCamera) {
		fImportView->AddHeaderButtons(fHeader);
	} else if (fItem != kItemPeople && !selection.empty()) {
		fHeader->AddButton("", kMsgToggleFavorite, kButtonHeart);
		if (selection.size() == 1)
			fHeader->AddButton("Edit", kMsgEdit, kButtonAccent);
		fHeader->AddButton("Delete", kMsgMoveToTrash, kButtonDanger);
	}
	fHeader->Finish();
}


void
MainWindow::_UpdateStatus()
{
	BString left;
	if (fViewerOpen) {
		int index = fGrid->IndexOf(fViewer->PhotoId());
		if (index >= 0) {
			left << Number(index + 1) << " of "
				<< Number((int)fGrid->Photos().size());
		}
	} else if (fCard == kCardGrid) {
		int total = (int)fGrid->Photos().size();
		int selected = fGrid->CountSelected();
		if (selected > 0)
			left << Number(selected) << " of " << CountText(total, "photo", "photos")
				<< " selected";
		else
			left = CountText(total, "photo", "photos");
	} else if (fCard == kCardPeople) {
		left << CountText(fPeopleView->CountPeople(), "person", "people") << ", "
			<< CountText(fPeopleView->CountGroups(), "group", "groups")
			<< " of unnamed faces";
	} else if (fCard == kCardImport)
		left = fImportView->StatusText();
	fStatus->SetLeft(left.String());

	// The most pressing thing that goes on.
	if (fBusyCount > 0 && fBusyText.Length() > 0)
		fStatus->SetRight(fBusyText.String());
	else if (fImportText.Length() > 0)
		fStatus->SetRight(fImportText.String(), fImportProgress);
	else {
		SyncState sync = fSession.GetSyncState();
		if (sync.running) {
			float progress = sync.total > 0 ? (float)sync.done / sync.total : -1;
			fStatus->SetRight(sync.text.c_str(), progress);
		} else if (fFacesText.Length() > 0)
			fStatus->SetRight(fFacesText.String());
		else
			fStatus->SetRight("");
	}
}


// --- The viewer -------------------------------------------------------------

void
MainWindow::_OpenViewer(int64_t id)
{
	if (id == 0)
		return;
	if (!fViewerOpen)
		fCardBeforeViewer = fCard == kCardViewer ? kCardGrid : fCard;
	fViewerOpen = true;
	fViewer->ShowPhoto(id, fGrid->TileSize());
	_ShowCard(kCardViewer);
	if (!fSuggestions->IsHidden())
		fSuggestions->Hide();
	if (fInfoWanted) {
		fInfo->SetPhoto(id);
		_SetSidePanel(1);
	} else
		_SetSidePanel(0);
	fViewer->MakeFocus(true);
	_UpdateHeader();
	_UpdateStatus();
}


void
MainWindow::_CloseViewer()
{
	if (fEdit->IsActive())
		_EndEdit(false);
	if (!fViewerOpen) {
		// "Back" from a person's photos leads to People.
		if (fItem == kItemPerson)
			_ShowView(kItemPeople, 0);
		return;
	}
	int64_t id = fViewer->PhotoId();
	fViewerOpen = false;
	fViewer->Clear();
	_SetSidePanel(0);
	Card back = fCardBeforeViewer == kCardViewer ? kCardGrid : fCardBeforeViewer;
	if (fItem == kItemPeople)
		back = kCardPeople;
	_ShowCard(back);
	if (back == kCardGrid) {
		if (fReloadPending) {
			fReloadPending = false;
			_ReloadList(true);
		}
		fGrid->Select(id, true);
		fGrid->MakeFocus(true);
		if (fItem == kItemPerson)
			fSuggestions->SetPerson(fArgument);
	}
	_UpdateHeader();
	_UpdateStatus();
}


void
MainWindow::_Step(int direction)
{
	if (!fViewerOpen || fEdit->IsActive())
		return;
	const std::vector<PhotoBrief>& photos = fGrid->Photos();
	int index = fGrid->IndexOf(fViewer->PhotoId());
	if (index < 0)
		return;
	index += direction;
	if (index < 0 || index >= (int)photos.size())
		return;
	bool faces = fViewer->ShowsFaces();
	fViewer->ShowPhoto(photos[index].id, fGrid->TileSize());
	fViewer->SetShowFaces(faces);
	if (fSidePanelShown == 1)
		fInfo->SetPhoto(photos[index].id);
	_UpdateHeader();
	_UpdateStatus();
}


std::vector<int64_t>
MainWindow::_Targets()
{
	std::vector<int64_t> targets;
	if (fViewerOpen) {
		if (fViewer->PhotoId() != 0)
			targets.push_back(fViewer->PhotoId());
	} else if (fCard == kCardGrid)
		targets = fGrid->Selection();
	return targets;
}


int64_t
MainWindow::_Target()
{
	std::vector<int64_t> targets = _Targets();
	return targets.size() == 1 ? targets[0] : 0;
}


// --- Editing ----------------------------------------------------------------

void
MainWindow::_BeginEdit()
{
	int64_t id = _Target();
	if (id == 0 || fEdit->IsActive())
		return;
	Photo photo;
	if (!fSession.Lib().Get(id, photo) || photo.trashed != 0)
		return;
	if (!fViewerOpen)
		_OpenViewer(id);
	fViewer->SetShowFaces(false);
	fViewer->ZoomToFit();
	fEdit->Begin(id);
	fEditSaving = false;
	fEditPanel->SetParams(fEdit->Params());
	fEditPanel->SetCropMode(false);
	fEditPanel->SetBusy(true, photo.path.empty() && photo.version == 0
		? "Getting the original from Immich…" : "Loading…");
	_SetSidePanel(2);
	_UpdateHeader();
}


void
MainWindow::_EndEdit(bool reload)
{
	if (!fEdit->IsActive())
		return;
	fEdit->End();
	fEditSaving = false;
	fViewer->EndEdit();
	if (fInfoWanted && fViewerOpen) {
		fInfo->SetPhoto(fViewer->PhotoId());
		_SetSidePanel(1);
	} else
		_SetSidePanel(0);
	if (reload)
		fViewer->Reload();
	fViewer->MakeFocus(true);
	_UpdateHeader();
}


void
MainWindow::_EditValuesChanged(BMessage* message)
{
	if (!fEdit->IsActive() || !fEdit->IsReady())
		return;
	EditParams params = fEdit->Params();
	if (fViewer->InCropMode())
		fViewer->GetCrop(params.cropX, params.cropY, params.cropW, params.cropH);
	int32 turn = message->GetInt32("turn", 0);
	if (turn != 0) {
		TurnEdit(params, turn);
		fEditPanel->SetParams(params);
	} else if (message->GetBool("flip", false)) {
		FlipEdit(params);
		fEditPanel->SetParams(params);
	} else {
		EditParams values = fEditPanel->Params(params);
		values.rotation = params.rotation;
		values.flip = params.flip;
		params = values;
	}
	fEdit->SetParams(params);
	if (fViewer->InCropMode() && (turn != 0 || message->GetBool("flip", false))) {
		// The frame turned with the picture; the new rendering shows it.
		fViewer->SetCropMode(true, params.cropX, params.cropY, params.cropW,
			params.cropH, 0);
	}
}


// --- Actions ----------------------------------------------------------------

void
MainWindow::_ToggleFavorite()
{
	std::vector<int64_t> targets = _Targets();
	if (targets.empty())
		return;
	Photo first;
	if (!fSession.Lib().Get(targets[0], first))
		return;
	bool favorite = !first.favorite;
	for (int64_t id : targets)
		fSession.Lib().SetFavorite(id, favorite);
	if (fSession.Sync().IsConfigured())
		fSession.SyncNow();
}


void
MainWindow::_Trash()
{
	std::vector<int64_t> targets = _Targets();
	if (targets.empty() || fItem == kItemTrash || fEdit->IsActive())
		return;
	if (fViewerOpen) {
		// On to the next photo, or back when it was the last.
		const std::vector<PhotoBrief>& photos = fGrid->Photos();
		int index = fGrid->IndexOf(targets[0]);
		int64_t next = 0;
		if (index >= 0 && index + 1 < (int)photos.size())
			next = photos[index + 1].id;
		else if (index > 0)
			next = photos[index - 1].id;
		fSession.Lib().MoveToTrash(targets);
		_ReloadList(true);
		if (next != 0) {
			fViewer->ShowPhoto(next, fGrid->TileSize());
			if (fSidePanelShown == 1)
				fInfo->SetPhoto(next);
			_UpdateHeader();
			_UpdateStatus();
		} else
			_CloseViewer();
		return;
	}
	fSession.Lib().MoveToTrash(targets);
}


void
MainWindow::_DeleteForever(bool all)
{
	std::vector<int64_t> targets;
	if (all) {
		std::vector<PhotoBrief> photos;
		fSession.Lib().List(kFilterTrash, 0, photos);
		for (auto& photo : photos)
			targets.push_back(photo.id);
	} else
		targets = _Targets();
	if (targets.empty())
		return;
	BString text;
	text << (targets.size() == 1 ? "This photo is"
		: (BString() << Number((int)targets.size()) << " photos are").String())
		<< " deleted for good, with all versions. This cannot be undone.";
	if (fSession.Sync().IsConfigured()) {
		text << (fSession.Prefs().GetBool(setting::kImmichTrashOnServer, false)
			? "\n\nOn Immich the photos move to the trash."
			: "\n\nPhotos that are on Immich stay there, but do not come back "
				"to this library.");
	}
	BAlert* alert = new BAlert("Delete", text.String(), "Cancel", "Delete", nullptr,
		B_WIDTH_AS_USUAL, B_WARNING_ALERT);
	alert->SetShortcut(0, B_ESCAPE);
	if (alert->Go() != 1)
		return;
	if (fViewerOpen)
		_CloseViewer();
	fSession.Lib().DeletePermanently(targets);
	if (fSession.Sync().IsConfigured())
		fSession.SyncNow();
}


void
MainWindow::_Rotate(int turns)
{
	std::vector<int64_t> targets = _Targets();
	if (targets.empty() || fEdit->IsActive() || fItem == kItemTrash)
		return;
	fBusyCount++;
	fBusyText = targets.size() == 1 ? "Rotating…" : "Rotating photos…";
	_UpdateStatus();
	Session* session = &fSession;
	BMessenger me(this);
	fSession.Async([session, me, targets, turns]() {
		std::string firstError;
		for (int64_t id : targets) {
			std::string error;
			if (!RotatePhoto(session->Lib(), session->Pics(), id, turns, error)
				&& firstError.empty())
				firstError = error;
		}
		BMessage done(kMsgBusyDone);
		if (!firstError.empty())
			done.AddString("error", firstError.c_str());
		me.SendMessage(&done);
	});
}


void
MainWindow::_OpenInEditor(const char* signature)
{
	int64_t id = _Target();
	if (id == 0 || fEdit->IsActive())
		return;
	Photo photo;
	if (!fSession.Lib().Get(id, photo))
		return;
	bool lightCraft = strcmp(signature, kLightCraftSignature) == 0;
	fBusyCount++;
	fBusyText = lightCraft ? "Opening in LightCraft…" : "Opening in PhotoCraft…";
	_UpdateStatus();
	Session* session = &fSession;
	BMessenger me(this);
	std::string sig = signature;
	fSession.Async([session, me, id, sig, lightCraft]() {
		std::string error;
		status_t status = B_OK;
		if (lightCraft) {
			// LightCraft develops the RAW file itself: it gets the original
			// (its own catalogue keeps its edits).
			Photo photo;
			if (!session->Sync().FetchOriginal(id, error)
				|| !session->Lib().Get(id, photo))
				status = B_ERROR;
			else {
				entry_ref ref;
				status = get_ref_for_path(session->Lib().OriginalPath(photo).c_str(),
					&ref);
				if (status == B_OK) {
					BMessage refs(B_REFS_RECEIVED);
					refs.AddRef("refs", &ref);
					status = be_roster->Launch(sig.c_str(), &refs);
					if (status == B_ALREADY_RUNNING)
						status = B_OK;
				}
				if (status != B_OK)
					error = "LightCraft could not be started.";
			}
		} else
			status = session->Editors().OpenInEditor(id, sig.c_str(), &error);
		BMessage done(kMsgBusyDone);
		if (status != B_OK)
			done.AddString("error", error.empty() ? strerror(status) : error.c_str());
		me.SendMessage(&done);
	});
}


void
MainWindow::_Export(BMessage* message)
{
	entry_ref ref;
	if (message->FindRef("refs", &ref) != B_OK)
		return;
	BPath path(&ref);
	std::vector<int64_t> targets = _Targets();
	if (targets.empty() || path.InitCheck() != B_OK)
		return;
	fBusyCount++;
	fBusyText = "Exporting…";
	_UpdateStatus();
	Session* session = &fSession;
	BMessenger me(this);
	std::string directory = path.Path();
	fSession.Async([session, me, targets, directory]() {
		int failed = 0;
		std::string firstError;
		for (int64_t id : targets) {
			std::string written, error;
			if (!ExportPhoto(session->Lib(), session->Pics(), id, directory, false,
					written, error)) {
				failed++;
				if (firstError.empty())
					firstError = error;
			}
		}
		BMessage done(kMsgBusyDone);
		if (failed > 0) {
			std::string text = std::to_string(failed) + (failed == 1 ? " photo"
				: " photos") + " could not be exported: " + firstError;
			done.AddString("error", text.c_str());
		}
		me.SendMessage(&done);
	});
}


void
MainWindow::ImportRefs(BMessage* message)
{
	std::vector<std::string> paths;
	entry_ref ref;
	for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
		BPath path(&ref);
		if (path.InitCheck() == B_OK)
			paths.push_back(path.Path());
	}
	if (paths.empty())
		return;
	fSession.Imports().ImportPaths(paths, "Files");
}


// --- People -----------------------------------------------------------------

void
MainWindow::_NamePerson(BMessage* message)
{
	std::string name = message->GetString("name", "");
	if (name.empty())
		return;
	int64_t cluster = Id(message, "cluster");
	int64_t face = Id(message, "face");
	int64_t person = Id(message, "person");
	fBusyCount++;
	fBusyText = "Looking for more of this person…";
	_UpdateStatus();
	Session* session = &fSession;
	BMessenger me(this);
	fSession.Async([session, me, name, cluster, face, person]() {
		if (cluster != 0)
			session->Persons().NameCluster(cluster, name);
		else if (face != 0)
			session->Persons().NameFace(face, name);
		else if (person != 0) {
			// A name someone else has: the two are the same person.
			int64_t other = session->Lib().FindPerson(name);
			if (other != 0 && other != person)
				session->Persons().MergePeople(person, other);
			else
				session->Lib().RenamePerson(person, name);
		}
		BMessage done(kMsgPeopleChanged);
		me.SendMessage(&done);
		BMessage busy(kMsgBusyDone);
		me.SendMessage(&busy);
	});
}


void
MainWindow::_PeopleChanged()
{
	if (fCard == kCardPeople)
		fPeopleView->Reload();
	if (fItem == kItemPerson && !fViewerOpen) {
		// The person may be gone (merged into another).
		Person person;
		if (!fSession.Lib().GetPerson(fArgument, person)) {
			_ShowView(kItemPeople, 0);
			return;
		}
		fSuggestions->SetPerson(fArgument);
		_ReloadList(true);
	}
	if (fViewerOpen) {
		fViewer->FacesChanged();
		if (fSidePanelShown == 1)
			fInfo->SetPhoto(fViewer->PhotoId());
	}
	_RebuildSidebar();
	_UpdateHeader();
	_UpdateStatus();
}


// --- Changes from outside ---------------------------------------------------

void
MainWindow::_LibraryChanged(BMessage* message)
{
	int64_t photoId = 0;
	uint32_t changes = fSession.TakeLibraryChanges(photoId);
	if (changes == 0)
		return;
	if ((changes & (kChangedList | kChangedPhoto)) != 0) {
		if (fCard == kCardGrid || fViewerOpen) {
			if (fViewerOpen && !fEdit->IsActive()) {
				// The list behind the viewer follows; the photo shown is
				// loaded again when it is the one that changed.
				_ReloadList(true);
				Photo photo;
				if (!fSession.Lib().Get(fViewer->PhotoId(), photo))
					_CloseViewer();
				else if ((changes & kChangedPhoto) != 0
					&& (photoId == 0 || photoId == fViewer->PhotoId())) {
					fViewer->Reload();
					if (fSidePanelShown == 1)
						fInfo->SetPhoto(fViewer->PhotoId());
				}
			} else if (fEdit->IsActive())
				fReloadPending = true;
			else
				_ReloadList(true);
		}
		if (photoId != 0)
			fSession.Pics().Invalidate(photoId);
	}
	if ((changes & kChangedPeople) != 0) {
		if (fCard == kCardPeople)
			fPeopleView->Reload();
		if (fViewerOpen) {
			fViewer->FacesChanged();
			if (fSidePanelShown == 1)
				fInfo->SetPhoto(fViewer->PhotoId());
		}
	}
	// Counts in the sidebar: not more often than twice a second.
	if (system_time() - fLastSidebar > 500000)
		_RebuildSidebar();
	else
		fLastSidebar = 1;	// the ticker catches up
	_UpdateHeader();
	_UpdateStatus();
}


void
MainWindow::CameraArrived(const char* key)
{
#ifdef PHOTOS_HAVE_CAMERAS
	_OpenCamera(key);
#else
	(void)key;
#endif
}


#ifdef PHOTOS_HAVE_CAMERAS
void
MainWindow::_OpenCamera(const char* key)
{
	std::vector<CameraInfo> cameras = fCameras->Cameras();
	fCameraKeys.clear();
	fCameraNames.clear();
	int index = -1;
	for (auto& camera : cameras) {
		if (key != nullptr && camera.key == key)
			index = (int)fCameraKeys.size();
		fCameraKeys.push_back(camera.key.c_str());
		fCameraNames.push_back(camera.name.c_str());
	}
	if (index < 0) {
		_RebuildSidebar();
		return;
	}
	if (fImportView->CameraKey() != key) {
		std::shared_ptr<CameraSource> source(fCameras->CreateSource(key));
		if (source)
			fImportView->SetCamera(source);
	}
	_ShowView(kItemCamera, index, true);
	_RebuildSidebar();
	if (IsMinimized())
		Minimize(false);
	Activate(true);
}
#endif


void
MainWindow::_AnswerTestQuery(BMessage* message)
{
	BMessage reply(B_REPLY);
	reply.AddInt32("item", fItem);
	reply.AddInt64("argument", fArgument);
	reply.AddInt32("card", fCard);
	reply.AddInt32("photos", (int32)fGrid->Photos().size());
	reply.AddInt32("selected", fGrid->CountSelected());
	reply.AddInt64("viewer", fViewerOpen ? fViewer->PhotoId() : 0);
	reply.AddBool("editing", fEdit->IsActive());
	reply.AddBool("edit ready", fEdit->IsReady());
	reply.AddInt32("mode", fGrid->Mode());
	reply.AddInt32("people", fPeopleView->CountPeople());
	reply.AddInt32("groups", fPeopleView->CountGroups());
	reply.AddInt32("cameras", (int32)fCameraKeys.size());
	reply.AddString("status", fImportView->StatusText());
	if (!fGrid->Photos().empty())
		reply.AddInt64("first", fGrid->Photos()[0].id);
	message->SendReply(&reply);
}


void
MainWindow::_SaveState()
{
	Settings& prefs = fSession.Prefs();
	BRect frame = Frame();
	BString text;
	text.SetToFormat("%.0f,%.0f,%.0f,%.0f", frame.left, frame.top, frame.right,
		frame.bottom);
	prefs.SetString(kFrameSetting, text.String());
	prefs.SetInt(kGroupModeSetting, fGroupMode);
	prefs.SetInt(setting::kTileSize, fGrid->TileSize());
	prefs.SetBool(kInfoSetting, fInfoWanted);
	prefs.Save();
}


bool
MainWindow::QuitRequested()
{
	_SaveState();
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}


void
MainWindow::MessageReceived(BMessage* message)
{
	if (message->WasDropped() && message->HasRef("refs")) {
		ImportRefs(message);
		return;
	}
	switch (message->what) {
		// --- From the services ---
		case kMsgLibraryChanged:
			_LibraryChanged(message);
			break;

		case kMsgTileReady:
		{
			int64_t face = Id(message, "face");
			if (face != 0) {
				if (fCard == kCardPeople)
					fPeopleView->TileReady(face);
				if (!fSuggestions->IsHidden())
					fSuggestions->TileReady(face);
				if (fSidePanelShown == 1)
					fInfo->TileReady(face);
			} else if (fCard == kCardGrid)
				fGrid->TileReady(Id(message, "id"));
			break;
		}

		case kMsgImportProgress:
		{
			bool running = message->GetBool("running", false);
			int32 done = message->GetInt32("done", 0);
			int32 total = message->GetInt32("total", 0);
			if (running) {
				fImportText = "";
				fImportText << "Importing " << Number(std::min(done + 1, total))
					<< " of " << Number(total);
				fImportProgress = total > 0 ? (float)done / total : -1;
			} else {
				fImportText = "";
				fImportProgress = -1;
				int32 added = message->GetInt32("added", 0);
				int32 duplicates = message->GetInt32("duplicates", 0);
				int32 failed = message->GetInt32("failed", 0);
				int64_t importId = Id(message, "import");
				fImportView->ImportFinished();
				_RebuildSidebar();
				if (added > 0 && importId != 0 && !fViewerOpen)
					_ShowView(kItemLastImport, importId, true);
				if (failed > 0) {
					BString text;
					text << CountText(failed, "picture", "pictures")
						<< " could not be imported.\n\n"
						<< message->GetString("error", "");
					_Alert("Import", text.String());
				} else if (added == 0 && duplicates > 0 && total > 0) {
					_Alert("Import", duplicates == 1
						? "This photo is in the library already."
						: "These photos are in the library already.");
				}
				if (fSession.Sync().IsConfigured() && added > 0)
					fSession.SyncNow();
			}
			_UpdateStatus();
			break;
		}

		case kMsgSyncStatus:
			_RebuildSidebar();
			_UpdateStatus();
			break;

		case kMsgFacesProgress:
		{
			fFacesText = "";
			if (message->GetBool("running", false)) {
				int32 remaining = message->GetInt32("remaining", 0);
				fFacesText << "Looking for faces · " << Number(remaining)
					<< (remaining == 1 ? " photo" : " photos") << " to go";
			}
			if (fCard == kCardPeople && !message->GetBool("running", false))
				fPeopleView->Reload();
			_UpdateStatus();
			break;
		}

		case kMsgVersionAdded:
		{
			const char* note = message->GetString("note", nullptr);
			if (note != nullptr)
				_Alert("PhotoCraft", note);
			break;
		}

		case kMsgPeopleChanged:
			_PeopleChanged();
			break;

		case photos::kMsgCameraArrived:
			// A device that was there when Photos started is only listed;
			// one that is plugged in now, or that the agent started Photos
			// for, is shown.
			if (message->GetBool("initial", false)
				&& !message->GetBool("launch", false)) {
#ifdef PHOTOS_HAVE_CAMERAS
				_OpenCamera(nullptr);
#endif
			} else
				CameraArrived(message->GetString("key", ""));
			break;

		case photos::kMsgCameraRemoved:
		{
#ifdef PHOTOS_HAVE_CAMERAS
			const char* key = message->GetString("key", "");
			fImportView->CameraRemoved(key);
			bool shown = fItem == kItemCamera && fArgument < (int64_t)fCameraKeys.size()
				&& fCameraKeys[fArgument] == key;
			_OpenCamera(nullptr);
			if (shown)
				_ShowView(kItemPhotos, 0, true);
#endif
			break;
		}

		case kMsgTick:
			if (fLastSidebar == 1)
				_RebuildSidebar();
			_UpdateStatus();
			break;

		case kMsgBusyDone:
		{
			fBusyCount = std::max(0, fBusyCount - 1);
			const char* error = message->GetString("error", nullptr);
			if (error != nullptr)
				_Alert("Photos", error);
			_UpdateStatus();
			break;
		}

		// --- Navigation ---
		case kMsgShowView:
		{
			int32 item = message->GetInt32("item", kItemPhotos);
			int64_t argument = Id(message, "argument");
#ifdef PHOTOS_HAVE_CAMERAS
			if (item == kItemCamera) {
				const char* key = message->GetString("key", nullptr);
				if (key == nullptr && argument < (int64_t)fCameraKeys.size())
					key = fCameraKeys[argument].String();
				if (key != nullptr)
					_OpenCamera(BString(key).String());
				break;
			}
#endif
			_ShowView(item, argument, true);
			break;
		}

		case kMsgGroupMode:
		{
			int32 mode = message->GetInt32("mode", kGroupDays);
			if (mode < kGroupYears || mode > kGroupNone)
				break;
			if (fItem != kItemPhotos || fViewerOpen)
				_ShowView(kItemPhotos, 0);
			fGroupMode = mode;
			fGrid->SetGroupMode((GroupMode)mode);
			int64_t time;
			if (message->FindInt64("time", &time) == B_OK)
				fGrid->ScrollToTime(time);
			_UpdateHeader();
			break;
		}

		case kMsgTileSize:
			fGrid->SetTileSize(message->GetInt32("size", fGrid->TileSize()));
			break;

		case kMsgZoomIn:
		case kMsgZoomOut:
			if (fViewerOpen)
				fViewer->ZoomBy(message->what == kMsgZoomIn ? 1.25f : 0.8f);
			else {
				fGrid->SetTileSize(fGrid->TileSize()
					+ (message->what == kMsgZoomIn ? 1 : -1) * (int)S(24));
				_UpdateHeader();
			}
			break;

		case kMsgZoomFit:
			fViewer->ZoomToFit();
			break;

		case kMsgZoomActual:
			fViewer->ZoomToActual();
			break;

		case kMsgOpenPhoto:
		{
			int64_t id = Id(message, "id");
			if (id == 0)
				id = _Target();
			_OpenViewer(id);
			break;
		}

		case kMsgCloseViewer:
			_CloseViewer();
			break;

		case kMsgPrevious:
			_Step(-1);
			break;

		case kMsgNext:
			_Step(1);
			break;

		case kMsgSelectionChanged:
			_UpdateHeader();
			_UpdateStatus();
			break;

		case kMsgSelectAll:
			if (fCard == kCardGrid)
				fGrid->SelectAll();
			else if (fCard == kCardImport)
				fImportView->SelectAll(true);
			break;

		case kMsgSelectNone:
			if (fCard == kCardGrid)
				fGrid->ClearSelection();
			else if (fCard == kCardImport)
				fImportView->SelectAll(false);
			break;

		// --- Photos ---
		case kMsgToggleFavorite:
			_ToggleFavorite();
			break;

		case kMsgMoveToTrash:
			_Trash();
			break;

		case kMsgRestore:
		{
			std::vector<int64_t> targets = _Targets();
			if (targets.empty())
				break;
			if (fViewerOpen)
				_CloseViewer();
			fSession.Lib().RestoreFromTrash(targets);
			break;
		}

		case kMsgDeleteForever:
			_DeleteForever(false);
			break;

		case kMsgEmptyTrash:
			_DeleteForever(true);
			break;

		case kMsgRotateLeft:
			_Rotate(-1);
			break;

		case kMsgRotateRight:
			_Rotate(1);
			break;

		case kMsgRevert:
		{
			int64_t id = _Target();
			if (id != 0 && !fEdit->IsActive())
				fSession.Lib().SetCurrentVersion(id, 0);
			break;
		}

		case kMsgShowVersion:
			if (fViewerOpen && !fEdit->IsActive()) {
				fSession.Lib().SetCurrentVersion(fViewer->PhotoId(),
					Id(message, "version"));
			}
			break;

		case kMsgDeleteVersion:
		{
			if (!fViewerOpen || fEdit->IsActive())
				break;
			BAlert* alert = new BAlert("Version",
				"Delete this version? The original and the other versions stay.",
				"Cancel", "Delete", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			if (alert->Go() == 1)
				fSession.Lib().DeleteVersion(Id(message, "version"));
			break;
		}

		case kMsgToggleInfo:
			if (!fViewerOpen || fEdit->IsActive())
				break;
			fInfoWanted = fSidePanelShown != 1;
			if (fInfoWanted) {
				fInfo->SetPhoto(fViewer->PhotoId());
				_SetSidePanel(1);
			} else
				_SetSidePanel(0);
			_UpdateHeader();
			break;

		case kMsgToggleFaces:
			if (fViewerOpen && !fEdit->IsActive()) {
				fViewer->SetShowFaces(!fViewer->ShowsFaces());
				_UpdateHeader();
			}
			break;

		case kMsgDownloadOriginal:
		{
			std::vector<int64_t> targets = _Targets();
			if (targets.empty())
				break;
			fBusyCount++;
			fBusyText = "Downloading from Immich…";
			_UpdateStatus();
			Session* session = &fSession;
			BMessenger me(this);
			fSession.Async([session, me, targets]() {
				std::string firstError;
				for (int64_t id : targets) {
					std::string error;
					if (!session->Sync().FetchOriginal(id, error) && firstError.empty())
						firstError = error;
				}
				BMessage done(kMsgBusyDone);
				if (!firstError.empty())
					done.AddString("error", firstError.c_str());
				me.SendMessage(&done);
			});
			break;
		}

		case kMsgShowInTracker:
		{
			Photo photo;
			int64_t id = _Target();
			if (id == 0 || !fSession.Lib().Get(id, photo) || photo.path.empty())
				break;
			BEntry entry(fSession.Lib().OriginalPath(photo).c_str());
			BEntry parent;
			entry_ref folder, file;
			if (entry.GetParent(&parent) == B_OK && parent.GetRef(&folder) == B_OK
				&& entry.GetRef(&file) == B_OK) {
				BMessage open(B_REFS_RECEIVED);
				open.AddRef("refs", &folder);
				BMessenger("application/x-vnd.Be-TRAK").SendMessage(&open);
			}
			break;
		}

		// --- Editing ---
		case kMsgEdit:
			_BeginEdit();
			break;

		case kMsgEditSourceReady:
			if (!fEdit->IsActive() || Id(message, "id") != fEdit->PhotoId())
				break;
			if (!message->GetBool("ok", false)) {
				_Alert("Edit", message->GetString("error", "The picture cannot be read."));
				_EndEdit(false);
				break;
			}
			fEditPanel->SetBusy(false, "");
			// From here on values render.
			fEdit->SourceReady();
			_UpdateHeader();
			break;

		case kMsgEditChanged:
			_EditValuesChanged(message);
			break;

		case kMsgEditCrop:
		{
			if (!fEdit->IsActive() || !fEdit->IsReady())
				break;
			bool on = message->GetBool("on", false);
			fEditPanel->SetCropMode(on);
			EditParams params = fEdit->Params();
			float aspect = fEditPanel->CropAspect();
			if (aspect < 0) {
				int width, height;
				fEdit->UncroppedSize(width, height);
				aspect = height > 0 ? (float)width / height : 0;
			}
			if (on && fViewer->InCropMode()) {
				// Only the shape changed.
				fViewer->SetCropAspect(aspect);
				break;
			}
			if (on) {
				fEdit->SetCropMode(true);
				fViewer->SetCropMode(true, params.cropX, params.cropY, params.cropW,
					params.cropH, aspect);
				if (message->GetBool("aspect", false))
					fViewer->SetCropAspect(aspect);
			} else {
				fViewer->GetCrop(params.cropX, params.cropY, params.cropW, params.cropH);
				fViewer->SetCropMode(false, 0, 0, 1, 1, 0);
				fEdit->SetParams(params);
				fEdit->SetCropMode(false);
			}
			break;
		}

		case kMsgCropChanged:
		{
			if (!fEdit->IsActive() || !fViewer->InCropMode())
				break;
			EditParams params = fEdit->Params();
			fViewer->GetCrop(params.cropX, params.cropY, params.cropW, params.cropH);
			// Kept; the frame is drawn by the viewer, nothing to render.
			fEdit->SetParamsQuietly(params);
			break;
		}

		case kMsgEditRendered:
		{
			BBitmap* bitmap = nullptr;
			message->FindPointer("bitmap", (void**)&bitmap);
			if (!fEdit->IsActive() || fEditSaving
				|| message->GetBool("crop", false) != fEdit->CropMode()) {
				delete bitmap;
				break;
			}
			fViewer->SetEditPicture(bitmap, message->GetInt32("width", 0),
				message->GetInt32("height", 0));
			break;
		}

		case kMsgEditEnhance:
		{
			if (!fEdit->IsActive() || !fEdit->IsReady())
				break;
			EditParams params = fEdit->Enhanced();
			if (fViewer->InCropMode())
				fViewer->GetCrop(params.cropX, params.cropY, params.cropW, params.cropH);
			fEditPanel->SetParams(params);
			fEdit->SetParams(params);
			break;
		}

		case kMsgEditReset:
		{
			if (!fEdit->IsActive() || !fEdit->IsReady())
				break;
			EditParams params;
			fEditPanel->SetParams(params);
			fEdit->SetParams(params);
			if (fViewer->InCropMode())
				fViewer->SetCropMode(true, 0, 0, 1, 1, 0);
			break;
		}

		case kMsgEditDone:
		{
			if (!fEdit->IsActive() || fEditSaving)
				break;
			if (!fEdit->IsReady()) {
				_EndEdit(false);
				break;
			}
			EditParams params = fEdit->Params();
			if (fViewer->InCropMode()) {
				fViewer->GetCrop(params.cropX, params.cropY, params.cropW, params.cropH);
				fEdit->SetParams(params);
			}
			if (!fEdit->IsChanged()) {
				_EndEdit(false);
				break;
			}
			fEditSaving = true;
			fEditPanel->SetBusy(true, "Saving…");
			_UpdateHeader();
			fEdit->Save();
			break;
		}

		case kMsgEditSaved:
			if (!fEdit->IsActive())
				break;
			if (!message->GetBool("ok", false)) {
				fEditSaving = false;
				fEditPanel->SetBusy(false, "");
				_UpdateHeader();
				_Alert("Edit", message->GetString("error", "The edit could not be saved."));
				break;
			}
			_EndEdit(true);
			break;

		case kMsgEditCancel:
			_EndEdit(false);
			break;

		case kMsgEditInPhotoCraft:
			_OpenInEditor(kPhotoCraftSignature);
			break;

		case kMsgOpenInLightCraft:
			_OpenInEditor(kLightCraftSignature);
			break;

		// --- Files ---
		case kMsgImportFiles:
			if (fOpenPanel == nullptr) {
				fOpenPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this), nullptr,
					B_FILE_NODE | B_DIRECTORY_NODE, true);
				fOpenPanel->Window()->SetTitle("Import pictures and folders");
				fOpenPanel->SetButtonLabel(B_DEFAULT_BUTTON, "Import");
			}
			fOpenPanel->Show();
			break;

		case B_REFS_RECEIVED:
			ImportRefs(message);
			break;

		case kMsgExport:
			if (_Targets().empty())
				break;
			if (fExportPanel == nullptr) {
				BMessage chosen(kMsgExportTo);
				fExportPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this),
					nullptr, B_DIRECTORY_NODE, false, &chosen);
				fExportPanel->Window()->SetTitle("Export to a folder");
				fExportPanel->SetButtonLabel(B_DEFAULT_BUTTON, "Export here");
			}
			fExportPanel->Show();
			break;

		case kMsgExportTo:
			_Export(message);
			break;

		case kMsgSyncNow:
			if (fSession.Sync().IsConfigured())
				fSession.SyncNow();
			else
				PostMessage(kMsgSettings);
			break;

		case kMsgSettings:
			if (fSettingsWindow != nullptr && fSettingsWindow->Lock()) {
				fSettingsWindow->Activate(true);
				fSettingsWindow->Unlock();
				break;
			}
			fSettingsWindow = new SettingsWindow(this, fSession);
			fSettingsWindow->Show();
			break;

		case kMsgSettingsChanged:
			_RebuildSidebar();
			_UpdateStatus();
			break;

		case kMsgAbout:
			be_app->PostMessage(B_ABOUT_REQUESTED);
			break;

		// --- People ---
		case kMsgOpenPerson:
			_ShowView(kItemPerson, Id(message, "person"), true);
			break;

		case kMsgNameCluster:
		case kMsgNameFace:
		case kMsgRenamePerson:
		{
			BMessage reply(kMsgNameChosen);
			BString initial;
			const char* title = "Who is this?";
			if (message->what == kMsgNameCluster)
				reply.AddInt64("cluster", Id(message, "cluster"));
			else if (message->what == kMsgNameFace) {
				int64_t faceId = Id(message, "face");
				reply.AddInt64("face", faceId);
				Face face;
				Person person;
				if (fSession.Lib().GetFace(faceId, face) && face.personId != 0
					&& fSession.Lib().GetPerson(face.personId, person))
					initial = person.name.c_str();
			} else {
				int64_t personId = Id(message, "person");
				reply.AddInt64("person", personId);
				Person person;
				if (fSession.Lib().GetPerson(personId, person))
					initial = person.name.c_str();
				title = "Rename (or merge with another person)";
			}
			(new NameWindow(this, fSession, title, initial.String(), reply))->Show();
			break;
		}

		case kMsgNameChosen:
			_NamePerson(message);
			break;

		case kMsgConfirmFace:
		case kMsgRejectFace:
		{
			int64_t face = Id(message, "face");
			int64_t person = Id(message, "person");
			bool confirm = message->what == kMsgConfirmFace;
			Session* session = &fSession;
			BMessenger me(this);
			fSession.Async([session, me, face, person, confirm]() {
				if (confirm)
					session->Persons().ConfirmFace(face, person);
				else
					session->Persons().RejectFace(face, person);
				me.SendMessage(kMsgPeopleChanged);
			});
			break;
		}

		case kMsgDeletePerson:
		{
			int64_t personId = Id(message, "person");
			Person person;
			if (!fSession.Lib().GetPerson(personId, person))
				break;
			BString text;
			text << "Forget " << person.name.c_str() << "? The photos stay; the faces "
				"go back to the unnamed ones.";
			BAlert* alert = new BAlert("Person", text.String(), "Cancel", "Forget",
				nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			if (alert->Go() != 1)
				break;
			Session* session = &fSession;
			BMessenger me(this);
			fSession.Async([session, me, personId]() {
				session->Persons().DeletePerson(personId);
				me.SendMessage(kMsgPeopleChanged);
			});
			break;
		}

		// --- Cameras ---
		case kMsgCameraListed:
		case kMsgCameraThumb:
			fImportView->MessageReceived(message);
			if (message->what == kMsgCameraListed) {
				_UpdateHeader();
				_UpdateStatus();
			}
			break;

		case kMsgCameraImportNew:
		case kMsgCameraImportSelected:
		case kMsgCameraImportAll:
			fImportView->StartImport(message->what == kMsgCameraImportNew ? 0
				: message->what == kMsgCameraImportSelected ? 1 : 2,
				fSession.Prefs().GetBool(setting::kDeleteAfterImport, false));
			_UpdateHeader();
			break;

		case kMsgTestQuery:
			_AnswerTestQuery(message);
			break;

		case kMsgTestSelect:
		{
			if (fCard != kCardGrid)
				break;
			int64_t id;
			fGrid->ClearSelection();
			for (int32 i = 0; message->FindInt64("id", i, &id) == B_OK; i++) {
				if (i == 0)
					fGrid->Select(id, true);
			}
			// hey sends numbers as int32.
			int32 small;
			if (message->FindInt32("id", &small) == B_OK)
				fGrid->Select(small, true);
			break;
		}

		case kMsgTestEdit:
		{
			EditParams params;
			if (!fEdit->IsActive() || !fEdit->IsReady()
				|| !EditParams::FromJson(message->GetString("json", ""), params))
				break;
			fEditPanel->SetParams(params);
			fEdit->SetParams(params);
			if (fViewer->InCropMode()) {
				fViewer->SetCropMode(true, params.cropX, params.cropY, params.cropW,
					params.cropH, 0);
			}
			break;
		}

		case B_COLORS_UPDATED:
			Theme::Refresh();
			for (BView* view : {(BView*)fSidebar, (BView*)fHeader, (BView*)fStatus,
					(BView*)fGrid, (BView*)fViewer, (BView*)fInfo,
					(BView*)fPeopleView, (BView*)fSuggestions, (BView*)fImportView})
				view->Invalidate();
			BWindow::MessageReceived(message);
			break;

		default:
			BWindow::MessageReceived(message);
	}
}

}	// namespace ui
