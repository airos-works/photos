// The Photos window: sidebar, header, the grid of the view chosen in the
// sidebar, the viewer and editor for one photo, people, a camera's pictures.
#pragma once

#include "Chrome.h"
#include "EditPanel.h"
#include "EditSession.h"
#include "GridView.h"
#include "InfoView.h"
#include "PeopleView.h"
#include "ViewerView.h"
#include "platform/Session.h"

#include <Window.h>

#include <memory>
#include <vector>

class BCardLayout;
class BFilePanel;
class BMenu;
class BMenuItem;
class BMessageRunner;

namespace ui {

class ImportView;

class MainWindow : public BWindow {
public:
	explicit MainWindow(photos::Session& session);
	virtual ~MainWindow();

	virtual void MessageReceived(BMessage* message);
	virtual bool QuitRequested();
	virtual void MenusBeginning();

	// Files and folders to bring into the library.
	void ImportRefs(BMessage* message);
	// A camera, phone or card arrived (from the watcher or photos_agent).
	void CameraArrived(const char* key);

private:
	enum Card { kCardGrid = 0, kCardViewer, kCardPeople, kCardImport };

	void _BuildMenus();
	void _ShowView(int item, int64_t argument, bool force = false);
	void _ReloadList(bool keepPosition);
	void _RebuildSidebar();
	void _UpdateHeader();
	void _UpdateStatus();
	void _ShowCard(Card card);
	void _OpenViewer(int64_t id);
	void _CloseViewer();
	void _Step(int direction);
	void _SetSidePanel(int panel);		// 0 none, 1 info, 2 edit
	std::vector<int64_t> _Targets();	// the photos an action applies to
	int64_t _Target();					// one of them
	void _BeginEdit();
	void _EndEdit(bool reload);
	void _EditValuesChanged(BMessage* message);
	void _ToggleFavorite();
	void _Trash();
	void _DeleteForever(bool all);
	void _Rotate(int turns);
	void _OpenInEditor(const char* signature);
	void _Export(BMessage* message);
	void _NamePerson(BMessage* message);
	void _PeopleChanged();
	void _LibraryChanged(BMessage* message);
	void _Alert(const char* title, const char* text);
	void _AnswerTestQuery(BMessage* message);
	void _SaveState();
#ifdef PHOTOS_HAVE_CAMERAS
	void _OpenCamera(const char* key);
#endif

	photos::Session& fSession;
	SidebarView* fSidebar;
	HeaderView* fHeader;
	StatusView* fStatus;
	SuggestionsView* fSuggestions;
	GridView* fGrid;
	ViewerView* fViewer;
	InfoView* fInfo;
	EditPanel* fEditPanel;
	PeopleView* fPeopleView;
	ImportView* fImportView;
	BCardLayout* fCards;
	BCardLayout* fSideCards;
	BView* fSidePanel;
	EditSession* fEdit;
	BFilePanel* fOpenPanel;
	BFilePanel* fExportPanel;
	BMessageRunner* fTicker;
	BWindow* fSettingsWindow;

	int fItem;
	int64_t fArgument;
	int fGroupMode;				// of the timeline
	Card fCard;
	Card fCardBeforeViewer;
	bool fViewerOpen;
	int fSidePanelShown;
	bool fInfoWanted;
	bool fEditSaving;
	bool fReloadPending;
	bigtime_t fLastSidebar;
	BString fBusyText;
	int fBusyCount;
	BString fImportText;
	float fImportProgress;
	BString fFacesText;
	std::vector<BString> fCameraKeys;
	std::vector<BString> fCameraNames;
#ifdef PHOTOS_HAVE_CAMERAS
	photos::CameraWatcher* fCameras;
#endif
};

}	// namespace ui
