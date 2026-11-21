#include "App.h"

#include "MainWindow.h"
#include "Ui.h"
#include "platform/ImageIO.h"
#include "platform/Messages.h"

#include <AboutWindow.h>
#include <Alert.h>
#include <Entry.h>
#include <Path.h>

#include <cstdio>
#include <cstring>

namespace ui {

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
			// Scripting ("hey Photos 'shVw' with item=3"): the window knows.
			if (fWindow != nullptr && message->what != B_ABOUT_REQUESTED
				&& (message->what >> 24) >= 'a' && (message->what >> 24) <= 'z') {
				if (message->what == kMsgTestQuery) {
					// The reply must come from the window to the sender.
					BMessage reply;
					if (BMessenger(fWindow).SendMessage(message, &reply, 3000000,
							3000000) == B_OK)
						message->SendReply(&reply);
					break;
				}
				BMessenger(fWindow).SendMessage(message);
				break;
			}
			BApplication::MessageReceived(message);
	}
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
