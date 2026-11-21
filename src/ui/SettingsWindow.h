// Settings: the Immich account and what is synced, importing, RAW files,
// faces.
#pragma once

#include "Ui.h"
#include "platform/Session.h"

#include <Window.h>

class BButton;
class BCheckBox;
class BMenuField;
class BStringView;
class BTextControl;

namespace ui {

class SettingsWindow : public BWindow {
public:
	SettingsWindow(BWindow* parent, photos::Session& session);

	virtual void MessageReceived(BMessage* message);
	virtual bool QuitRequested();

private:
	void _UpdateAccount();
	void _Apply();

	photos::Session& fSession;
	BMessenger fParent;
	BTextControl* fServer;
	BTextControl* fEmail;
	BTextControl* fPassword;
	BButton* fSignIn;
	BButton* fSignOut;
	BStringView* fAccount;
	BCheckBox* fUpload;
	BCheckBox* fKeepOriginals;
	BCheckBox* fTrashOnServer;
	BMenuField* fInterval;
	BCheckBox* fOpenOnConnect;
	BCheckBox* fDeleteAfterImport;
	BCheckBox* fRawPreview;
	BCheckBox* fFaces;
	BCheckBox* fFacesCpu;
	BStringView* fFacesStatus;
	BStringView* fLibrary;
	bool fSigningIn;
};

}	// namespace ui
