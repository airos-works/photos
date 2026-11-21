// The editor's work behind the window: the picture being edited at preview
// size, and a thread that renders the adjustments as the sliders move (the
// newest values win; values in between are skipped).
#pragma once

#include "Ui.h"
#include "core/Edit.h"
#include "platform/Editing.h"
#include "platform/Session.h"

#include <Messenger.h>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace ui {

class EditSession {
public:
	EditSession(photos::Session& session, const BMessenger& target);
	~EditSession();

	// Starts editing a photo: the preview is loaded on the thread, then
	// kMsgEditSourceReady arrives.
	void Begin(int64_t photoId);
	void End();
	bool IsActive() const { return fPhotoId != 0; }
	bool IsReady() const { return fReady; }
	int64_t PhotoId() const { return fPhotoId; }

	const photos::EditParams& Params() const { return fParams; }
	bool IsChanged() const { return fParams != fInitial; }
	// New values: a rendering follows as kMsgEditRendered ("bitmap",
	// "width", "height": the size of the full edited picture).
	void SetParams(const photos::EditParams& params);
	// The same without a rendering (the crop frame moved: the viewer draws
	// it).
	void SetParamsQuietly(const photos::EditParams& params);
	// The preview is loaded (kMsgEditSourceReady came): render it.
	void SourceReady();
	void SetCropMode(bool cropMode);
	bool CropMode() const { return fCropMode; }
	// Suitable light and colour values for the picture.
	photos::EditParams Enhanced();
	// The size of the picture the crop frame is drawn on.
	void UncroppedSize(int& width, int& height) const;
	void BaseSize(int& width, int& height) const;

	// Renders at full size and stores the version, on the session's worker;
	// kMsgEditSaved follows.
	void Save();

private:
	void _Run();
	void _Request();

	photos::Session& fSession;
	BMessenger fTarget;
	int64_t fPhotoId;
	photos::EditTarget fEditTarget;
	photos::EditParams fParams;
	photos::EditParams fInitial;
	bool fCropMode;
	bool fReady;

	std::mutex fLock;
	std::condition_variable fWake;
	std::thread fThread;
	photos::Image fSource;			// guarded by fLock
	int64_t fLoadPhoto;				// a photo to load the preview of
	bool fRenderWanted;
	photos::EditParams fRenderParams;
	bool fRenderCrop;
	int32 fSerial;
	bool fQuit;
};

}	// namespace ui
