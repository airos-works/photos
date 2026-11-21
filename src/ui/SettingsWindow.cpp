#include "SettingsWindow.h"

#include "platform/ImageIO.h"

#include <Box.h>
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <SeparatorView.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>

using namespace photos;

namespace ui {

namespace {

const uint32 kMsgSignIn = 'swSi';
const uint32 kMsgSignedIn = 'swSd';
const uint32 kMsgSignOut = 'swSo';
const uint32 kMsgOption = 'swOp';
const uint32 kMsgInterval = 'swIn';

const int kIntervals[] = {5, 15, 30, 60, 240};


BStringView*
Note(const char* text)
{
	BStringView* view = new BStringView("note", text);
	view->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DISABLED_LABEL_TINT);
	BFont font(be_plain_font);
	font.SetSize(be_plain_font->Size() * 0.92f);
	view->SetFont(&font);
	return view;
}

}	// namespace


SettingsWindow::SettingsWindow(BWindow* parent, Session& session)
	:
	BWindow(BRect(0, 0, S(520), S(420)), "Photos settings", B_TITLED_WINDOW,
		B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS
			| B_CLOSE_ON_ESCAPE | B_ASYNCHRONOUS_CONTROLS),
	fSession(session),
	fParent(parent),
	fSigningIn(false)
{
	Settings& prefs = session.Prefs();

	// --- Immich ---
	fServer = new BTextControl("server", "Server:",
		prefs.GetString(setting::kImmichServer).c_str(), nullptr);
	fEmail = new BTextControl("email", "E-mail:",
		prefs.GetString(setting::kImmichUser).c_str(), nullptr);
	fPassword = new BTextControl("password", "Password:", "", new BMessage(kMsgSignIn));
	fPassword->TextView()->HideTyping(true);
	fSignIn = new BButton("sign in", "Sign in", new BMessage(kMsgSignIn));
	fSignOut = new BButton("sign out", "Sign out", new BMessage(kMsgSignOut));
	fAccount = new BStringView("account", "");
	fUpload = new BCheckBox("upload",
		"Send photos that are not on the server to Immich", new BMessage(kMsgOption));
	fKeepOriginals = new BCheckBox("originals",
		"Keep the originals of all Immich photos on this computer",
		new BMessage(kMsgOption));
	fTrashOnServer = new BCheckBox("trash",
		"Photos deleted here for good also go to the trash on Immich",
		new BMessage(kMsgOption));
	BPopUpMenu* intervals = new BPopUpMenu("interval");
	int64_t current = prefs.GetInt(setting::kImmichInterval, 15);
	for (int minutes : kIntervals) {
		BString label;
		if (minutes < 60)
			label.SetToFormat("%d minutes", minutes);
		else
			label.SetToFormat(minutes == 60 ? "hour" : "%d hours", minutes / 60);
		BMessage* message = new BMessage(kMsgInterval);
		message->AddInt32("minutes", minutes);
		BMenuItem* item = new BMenuItem(label.String(), message);
		item->SetMarked(minutes == current);
		intervals->AddItem(item);
	}
	fInterval = new BMenuField("interval", "Look for changes every", intervals);

	BView* immich = new BView("Immich", 0);
	immich->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	BLayoutBuilder::Group<>(immich, B_VERTICAL, S(6))
		.SetInsets(S(14))
		.AddGrid(S(8), S(6))
			.Add(fServer->CreateLabelLayoutItem(), 0, 0)
			.Add(fServer->CreateTextViewLayoutItem(), 1, 0)
			.Add(fEmail->CreateLabelLayoutItem(), 0, 1)
			.Add(fEmail->CreateTextViewLayoutItem(), 1, 1)
			.Add(fPassword->CreateLabelLayoutItem(), 0, 2)
			.Add(fPassword->CreateTextViewLayoutItem(), 1, 2)
		.End()
		.Add(Note("For example http://192.168.1.10:2283. The password is not "
			"kept, only the session the server gives."))
		.AddGroup(B_HORIZONTAL, S(8))
			.Add(fAccount)
			.AddGlue()
			.Add(fSignOut)
			.Add(fSignIn)
		.End()
		.Add(new BSeparatorView(B_HORIZONTAL))
		.Add(fUpload)
		.Add(fKeepOriginals)
		.Add(Note("Otherwise an original is fetched when you open, edit or "
			"export its photo."))
		.Add(fTrashOnServer)
		.AddGroup(B_HORIZONTAL, S(6))
			.Add(fInterval)
			.AddGlue()
		.End()
		.AddGlue();

	// --- Import and files ---
	fOpenOnConnect = new BCheckBox("open",
		"Open Photos when a camera, phone or memory card is connected",
		new BMessage(kMsgOption));
	fDeleteAfterImport = new BCheckBox("delete",
		"Delete pictures from the device after they are imported",
		new BMessage(kMsgOption));
	fRawPreview = new BCheckBox("raw",
		"Show RAW files with the camera's own preview instead of developing them",
		new BMessage(kMsgOption));
	fLibrary = new BStringView("library", session.Lib().Directory().c_str());
	BString engine("RAW files are developed with the LightCraft engine");
	if (!HaveRawEngine())
		engine = "This build shows RAW files with their embedded previews only.";
	BView* files = new BView("Import", 0);
	files->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	BLayoutBuilder::Group<>(files, B_VERTICAL, S(6))
		.SetInsets(S(14))
		.Add(fOpenOnConnect)
		.Add(fDeleteAfterImport)
		.Add(new BSeparatorView(B_HORIZONTAL))
		.Add(fRawPreview)
		.Add(Note(engine.String()))
		.Add(new BSeparatorView(B_HORIZONTAL))
		.Add(new BStringView("label", "The library is in:"))
		.Add(fLibrary)
		.AddGlue();

	// --- Faces ---
	fFaces = new BCheckBox("faces", "Find the faces in my photos",
		new BMessage(kMsgOption));
	fFacesCpu = new BCheckBox("cpu",
		"Use the processor only (not the graphics or neural processor)",
		new BMessage(kMsgOption));
	fFacesStatus = new BStringView("status", "");
	BView* faces = new BView("People", 0);
	faces->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	BLayoutBuilder::Group<>(faces, B_VERTICAL, S(6))
		.SetInsets(S(14))
		.Add(fFaces)
		.Add(Note("Faces are found and compared on this computer with the "
			"Neural Kit. Nothing about them is sent anywhere."))
		.Add(fFacesCpu)
		.Add(new BSeparatorView(B_HORIZONTAL))
		.Add(fFacesStatus)
		.AddGlue();

	BTabView* tabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);
	tabs->AddTab(immich);
	tabs->AddTab(files);
	tabs->AddTab(faces);
	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.SetInsets(0, S(8), 0, 0)
		.Add(tabs);

	fUpload->SetValue(prefs.GetBool(setting::kImmichUpload, true));
	fKeepOriginals->SetValue(prefs.GetBool(setting::kImmichKeepOriginals, false));
	fTrashOnServer->SetValue(prefs.GetBool(setting::kImmichTrashOnServer, false));
	fOpenOnConnect->SetValue(prefs.GetBool(setting::kOpenOnConnect, true));
	fDeleteAfterImport->SetValue(prefs.GetBool(setting::kDeleteAfterImport, false));
	fRawPreview->SetValue(prefs.GetBool(setting::kRawUsePreview, false));
	fFaces->SetValue(prefs.GetBool(setting::kFacesEnabled, true));
	fFacesCpu->SetValue(prefs.GetBool(setting::kFacesCpuOnly, false));
	_UpdateAccount();
	CenterIn(parent->Frame());
}


void
SettingsWindow::_UpdateAccount()
{
	Settings& prefs = fSession.Prefs();
	bool signedIn = fSession.Sync().IsConfigured();
	BString text;
	if (fSigningIn)
		text = "Signing in…";
	else if (signedIn) {
		text << "Signed in as " << prefs.GetString(setting::kImmichUser).c_str();
	} else
		text = "Not signed in";
	fAccount->SetText(text.String());
	fSignOut->SetEnabled(signedIn && !fSigningIn);
	fSignIn->SetEnabled(!fSigningIn);
	fSignIn->SetLabel(signedIn ? "Sign in again" : "Sign in");

	FaceScanStatus scan = fSession.Faces().Status();
	BString faces;
	if (!scan.error.empty())
		faces = scan.error.c_str();
	else if (FaceScanner::ModelVersion() == 0)
		faces = "This build has no face engine.";
	else {
		faces << Number(fSession.Persons().CountFaces()) << " faces known";
		if (!scan.device.empty())
			faces << "  ·  running on " << scan.device.c_str();
		if (scan.running)
			faces << "  ·  " << Number(scan.remaining) << " photos to go";
	}
	fFacesStatus->SetText(faces.String());
}


void
SettingsWindow::_Apply()
{
	Settings& prefs = fSession.Prefs();
	prefs.SetBool(setting::kImmichUpload, fUpload->Value() == B_CONTROL_ON);
	prefs.SetBool(setting::kImmichKeepOriginals,
		fKeepOriginals->Value() == B_CONTROL_ON);
	prefs.SetBool(setting::kImmichTrashOnServer,
		fTrashOnServer->Value() == B_CONTROL_ON);
	prefs.SetBool(setting::kOpenOnConnect, fOpenOnConnect->Value() == B_CONTROL_ON);
	prefs.SetBool(setting::kDeleteAfterImport,
		fDeleteAfterImport->Value() == B_CONTROL_ON);
	prefs.SetBool(setting::kRawUsePreview, fRawPreview->Value() == B_CONTROL_ON);
	bool faces = fFaces->Value() == B_CONTROL_ON;
	bool cpu = fFacesCpu->Value() == B_CONTROL_ON;
	bool facesChanged = faces != prefs.GetBool(setting::kFacesEnabled, true)
		|| cpu != prefs.GetBool(setting::kFacesCpuOnly, false);
	prefs.SetBool(setting::kFacesEnabled, faces);
	prefs.SetBool(setting::kFacesCpuOnly, cpu);
	prefs.Save();
	fSession.ApplySyncSettings();
	fSession.ApplyDeviceSettings();
	if (facesChanged)
		fSession.ApplyFaceSettings();
	fParent.SendMessage(kMsgSettingsChanged);
}


void
SettingsWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgOption:
			_Apply();
			break;

		case kMsgInterval:
			fSession.Prefs().SetInt(setting::kImmichInterval,
				message->GetInt32("minutes", 15));
			fSession.Prefs().Save();
			break;

		case kMsgSignIn:
		{
			if (fSigningIn)
				break;
			std::string server = fServer->Text();
			std::string email = fEmail->Text();
			std::string password = fPassword->Text();
			if (server.empty() || email.empty() || password.empty()) {
				fAccount->SetText("Enter the server, your e-mail address and password.");
				break;
			}
			fSigningIn = true;
			_UpdateAccount();
			Session* session = &fSession;
			BMessenger me(this);
			fSession.Async([session, me, server, email, password]() {
				std::string error;
				bool ok = session->SignIn(server, email, password, error);
				BMessage done(kMsgSignedIn);
				done.AddBool("ok", ok);
				done.AddString("error", error.c_str());
				me.SendMessage(&done);
			});
			break;
		}

		case kMsgSignedIn:
			fSigningIn = false;
			fPassword->SetText("");
			_UpdateAccount();
			if (message->GetBool("ok", false)) {
				fServer->SetText(
					fSession.Prefs().GetString(setting::kImmichServer).c_str());
				fSession.SyncNow();
			} else
				fAccount->SetText(message->GetString("error", "Could not sign in."));
			fParent.SendMessage(kMsgSettingsChanged);
			break;

		case kMsgSignOut:
			fSession.SignOut();
			_UpdateAccount();
			fParent.SendMessage(kMsgSettingsChanged);
			break;

		default:
			BWindow::MessageReceived(message);
	}
}


bool
SettingsWindow::QuitRequested()
{
	return true;
}

}	// namespace ui
