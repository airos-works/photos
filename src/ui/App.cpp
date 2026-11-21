#include "App.h"

#include "MainWindow.h"
#include "Ui.h"
#include "platform/ImageIO.h"
#include "platform/Messages.h"

#include <AboutWindow.h>
#include <Alert.h>
#include <Entry.h>
#include <Path.h>
#include <PropertyInfo.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ui {

namespace {

// Commands for scripting: "hey Photos showview with item=3", "hey Photos
// state". Fields go in with "with name=value and name=value". (No command
// has four letters: hey takes such a word for a message code.)
value_info sCommands[] = {
	{"state", kMsgTestQuery, B_COMMAND_KIND, "the window's state, as a reply"},
	{"showview", kMsgShowView, B_COMMAND_KIND,
		"item=0 photos, 1 recents, 2 favorites, 3 people, 4 edited, 5 last import, "
		"6 trash, 7 person (argument=id), 8 camera (argument=index)"},
	{"group", kMsgGroupMode, B_COMMAND_KIND, "mode=0 years, 1 months, 2 days, 3 all"},
	{"tilesize", kMsgTileSize, B_COMMAND_KIND, "size=pixels"},
	{"select", kMsgTestSelect, B_COMMAND_KIND, "id=photo (several allowed)"},
	{"openphoto", kMsgOpenPhoto, B_COMMAND_KIND, "id=photo (else the selected one)"},
	{"close", kMsgCloseViewer, B_COMMAND_KIND, "back to the grid"},
	{"previousphoto", kMsgPrevious, B_COMMAND_KIND, ""},
	{"nextphoto", kMsgNext, B_COMMAND_KIND, ""},
	{"favorite", kMsgToggleFavorite, B_COMMAND_KIND, ""},
	{"trash", kMsgMoveToTrash, B_COMMAND_KIND, ""},
	{"restore", kMsgRestore, B_COMMAND_KIND, ""},
	{"rotate", kMsgRotateRight, B_COMMAND_KIND, ""},
	{"revert", kMsgRevert, B_COMMAND_KIND, ""},
	{"version", kMsgShowVersion, B_COMMAND_KIND, "version=id (0: the original)"},
	{"showinfo", kMsgToggleInfo, B_COMMAND_KIND, ""},
	{"faces", kMsgToggleFaces, B_COMMAND_KIND, ""},
	{"editphoto", kMsgEdit, B_COMMAND_KIND, "starts editing the photo shown or selected"},
	{"editvalues", kMsgTestEdit, B_COMMAND_KIND, "json=the editor's values"},
	{"editcrop", kMsgEditCrop, B_COMMAND_KIND, "on=true or false"},
	{"enhance", kMsgEditEnhance, B_COMMAND_KIND, ""},
	{"editdone", kMsgEditDone, B_COMMAND_KIND, ""},
	{"editcancel", kMsgEditCancel, B_COMMAND_KIND, ""},
	{"photocraft", kMsgEditInPhotoCraft, B_COMMAND_KIND, ""},
	{"syncnow", kMsgSyncNow, B_COMMAND_KIND, ""},
	{"settings", kMsgSettings, B_COMMAND_KIND, ""},
	{"person", kMsgOpenPerson, B_COMMAND_KIND, "person=id"},
	{"namepeople", kMsgNameChosen, B_COMMAND_KIND, "name=..., and cluster=, face= or person="},
	{"confirm", kMsgConfirmFace, B_COMMAND_KIND, "face=, person="},
	{"reject", kMsgRejectFace, B_COMMAND_KIND, "face=, person="},
	{"import-new", kMsgCameraImportNew, B_COMMAND_KIND, "from the camera shown"},
	{"import-all", kMsgCameraImportAll, B_COMMAND_KIND, "from the camera shown"},
	{0}
};

}	// namespace


App::App()
	:
	BApplication("application/x-vnd.airos-Photos"),
	fWindow(nullptr),
	fPendingRefs(B_REFS_RECEIVED),
	fOpened(false)
{
}


App::~App()
{
}


void
App::ReadyToRun()
{
	std::string error;
	if (!fSession.Open(&error)) {
		BString text("The photo library could not be opened.\n\n");
		text << error.c_str();
		(new BAlert("Photos", text.String(), "Quit", nullptr, nullptr,
			B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go();
		PostMessage(B_QUIT_REQUESTED);
		return;
	}
	fOpened = true;
	fWindow = new MainWindow(fSession);
	fSession.Start(BMessenger(fWindow));
	fWindow->Show();
	if (fPendingRefs.HasRef("refs"))
		BMessenger(fWindow).SendMessage(&fPendingRefs);
	if (fPendingCamera.Length() > 0) {
		BMessage arrived(photos::kMsgCameraArrived);
		arrived.AddString("key", fPendingCamera);
		BMessenger(fWindow).SendMessage(&arrived);
	}
}


void
App::RefsReceived(BMessage* message)
{
	if (fWindow == nullptr) {
		// Before the window: kept for ReadyToRun.
		entry_ref ref;
		for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++)
			fPendingRefs.AddRef("refs", &ref);
		return;
	}
	BMessenger(fWindow).SendMessage(message);
}


void
App::ArgvReceived(int32 argc, char** argv)
{
	// Paths of pictures and folders to import.
	BMessage refs(B_REFS_RECEIVED);
	const char* cwd = nullptr;
	if (CurrentMessage() != nullptr)
		CurrentMessage()->FindString("cwd", &cwd);
	for (int32 i = 1; i < argc; i++) {
		if (argv[i][0] == '-')
			continue;
		BPath path;
		if (argv[i][0] == '/' || cwd == nullptr)
			path.SetTo(argv[i]);
		else
			path.SetTo(cwd, argv[i]);
		entry_ref ref;
		if (get_ref_for_path(path.Path(), &ref) == B_OK)
			refs.AddRef("refs", &ref);
	}
	if (refs.HasRef("refs"))
		RefsReceived(&refs);
}


void
App::MessageReceived(BMessage* message)
{
	if (getenv("PHOTOS_TRACE") != nullptr) {
		fprintf(stderr, "app message '%c%c%c%c'\n", (char)(message->what >> 24),
			(char)(message->what >> 16), (char)(message->what >> 8),
			(char)message->what);
		char* name;
		type_code type;
		int32 count;
		for (int32 i = 0; message->GetInfo(B_ANY_TYPE, i, &name, &type, &count) == B_OK;
				i++) {
			fprintf(stderr, "  %s: '%c%c%c%c' x %d\n", name, (char)(type >> 24),
				(char)(type >> 16), (char)(type >> 8), (char)type, (int)count);
		}
	}
	switch (message->what) {
		case photos::kMsgCameraArrived:
			// From photos_agent: a camera was connected.
			if (fWindow == nullptr) {
				fPendingCamera = message->GetString("key", "");
				break;
			}
			BMessenger(fWindow).SendMessage(message);
			break;

		case B_SILENT_RELAUNCH:
			if (fWindow != nullptr)
				fWindow->Activate(true);
			break;

		default:
			// Scripting ("hey Photos showview with item=3"): the window
			// knows. A copy goes there: sending the message itself would
			// overwrite where its answer has to go.
			if (fWindow != nullptr && message->what != B_ABOUT_REQUESTED
				&& (message->what >> 24) >= 'a' && (message->what >> 24) <= 'z') {
				BMessage copy(*message);
				if (message->what == kMsgTestQuery) {
					BMessage reply;
					if (BMessenger(fWindow).SendMessage(&copy, &reply, 3000000,
							3000000) == B_OK)
						message->SendReply(&reply);
					break;
				}
				BMessenger(fWindow).SendMessage(&copy);
				if (message->IsSourceWaiting())
					message->SendReply(B_REPLY);
				break;
			}
			BApplication::MessageReceived(message);
	}
}


status_t
App::GetSupportedSuites(BMessage* data)
{
	data->AddString("suites", "suite/vnd.airos-Photos");
	BPropertyInfo info(nullptr, sCommands);
	data->AddFlat("messages", &info);
	return BApplication::GetSupportedSuites(data);
}


void
App::AboutRequested()
{
	BAboutWindow* about = new BAboutWindow("Photos", "application/x-vnd.airos-Photos");
	BString description("Your photo library for air/OS: by time and by people, "
		"with basic edits, cameras and phones, and Immich.");
	std::string engine = photos::RawEngineVersion();
	if (!engine.empty())
		description << "\n\nRAW engine: " << engine.c_str();
	about->AddDescription(description.String());
	about->AddCopyright(2026, "air/OS contributors");
	about->Show();
}


bool
App::QuitRequested()
{
	if (fWindow != nullptr && fWindow->Lock()) {
		// The window saves its state when it quits.
		fWindow->Quit();
		fWindow = nullptr;
	}
	if (fOpened)
		fSession.Stop();
	return true;
}

}	// namespace ui


int
main()
{
	ui::App app;
	app.Run();
	return 0;
}
