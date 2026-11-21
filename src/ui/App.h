// The Photos application: one window on one library.
#pragma once

#include "platform/Session.h"

#include <Application.h>

namespace ui {

class MainWindow;

class App : public BApplication {
public:
	App();
	virtual ~App();

	virtual void ReadyToRun();
	virtual void RefsReceived(BMessage* message);
	virtual void ArgvReceived(int32 argc, char** argv);
	virtual void MessageReceived(BMessage* message);
	virtual void AboutRequested();
	virtual bool QuitRequested();

private:
	photos::Session fSession;
	MainWindow* fWindow;
	BMessage fPendingRefs;
	BString fPendingCamera;
	bool fOpened;
};

}	// namespace ui
