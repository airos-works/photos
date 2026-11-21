#include "EditSession.h"

#include "platform/ImageIO.h"

#include <Bitmap.h>
#include <Message.h>

using namespace photos;

namespace ui {

namespace {

// The longer edge of the picture the sliders work on.
const int kPreviewEdge = 2048;

}	// namespace


EditSession::EditSession(Session& session, const BMessenger& target)
	:
	fSession(session),
	fTarget(target),
	fPhotoId(0),
	fCropMode(false),
	fReady(false),
	fLoadPhoto(0),
	fRenderWanted(false),
	fRenderCrop(false),
	fSerial(0),
	fQuit(false)
{
	fThread = std::thread([this]() { _Run(); });
}


EditSession::~EditSession()
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		fQuit = true;
	}
	fWake.notify_all();
	fThread.join();
}


void
EditSession::Begin(int64_t photoId)
{
	Photo photo;
	if (!fSession.Lib().Get(photoId, photo))
		return;
	GetEditTarget(fSession.Lib(), photo, fEditTarget);
	fParams = fInitial = fEditTarget.params;
	fPhotoId = photoId;
	fCropMode = false;
	fReady = false;
	{
		std::lock_guard<std::mutex> lock(fLock);
		fSource.Clear();
		fLoadPhoto = photoId;
		fRenderWanted = false;
		fSerial++;
	}
	fWake.notify_all();
}


void
EditSession::End()
{
	fPhotoId = 0;
	fReady = false;
	std::lock_guard<std::mutex> lock(fLock);
	fSource.Clear();
	fLoadPhoto = 0;
	fRenderWanted = false;
	fSerial++;
}


void
EditSession::_Request()
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		fRenderParams = fParams;
		fRenderCrop = fCropMode;
		fRenderWanted = true;
		fSerial++;
	}
	fWake.notify_all();
}


void
EditSession::SetParams(const EditParams& params)
{
	fParams = params;
	ClampCrop(fParams);
	if (fReady)
		_Request();
}


void
EditSession::SetCropMode(bool cropMode)
{
	fCropMode = cropMode;
	if (fReady)
		_Request();
}


EditParams
EditSession::Enhanced()
{
	std::lock_guard<std::mutex> lock(fLock);
	if (fSource.IsEmpty())
		return fParams;
	// Judged on a small copy of the picture as it is framed now.
	Image small;
	EditParams geometry = fParams;
	geometry.exposure = geometry.contrast = geometry.highlights = 0;
	geometry.shadows = geometry.saturation = geometry.warmth = geometry.tint = 0;
	ApplyGeometry(fSource, geometry, small, 400);
	return AutoEnhance(small, fParams);
}


void
EditSession::BaseSize(int& width, int& height) const
{
	width = fEditTarget.baseWidth;
	height = fEditTarget.baseHeight;
}


void
EditSession::UncroppedSize(int& width, int& height) const
{
	photos::UncroppedSize(fEditTarget.baseWidth, fEditTarget.baseHeight, fParams,
		width, height);
}


void
EditSession::Save()
{
	int64_t photoId = fPhotoId;
	EditParams params = fParams;
	BMessenger target = fTarget;
	Session* session = &fSession;
	session->Async([session, photoId, params, target]() {
		std::string error;
		bool ok = SaveEdit(session->Lib(), session->Pics(), photoId, params, error);
		BMessage message(kMsgEditSaved);
		message.AddInt64("id", photoId);
		message.AddBool("ok", ok);
		if (!ok)
			message.AddString("error", error.c_str());
		target.SendMessage(&message);
	});
}


void
EditSession::_Run()
{
	for (;;) {
		int64_t load = 0;
		EditParams params;
		bool crop = false;
		int32 serial = 0;
		{
			std::unique_lock<std::mutex> lock(fLock);
			fWake.wait(lock, [this]() {
				return fQuit || fLoadPhoto != 0 || fRenderWanted;
			});
			if (fQuit)
				return;
			if (fLoadPhoto != 0) {
				load = fLoadPhoto;
				fLoadPhoto = 0;
			} else {
				params = fRenderParams;
				crop = fRenderCrop;
				fRenderWanted = false;
			}
			serial = fSerial;
		}

		if (load != 0) {
			Photo photo;
			Image image;
			std::string error;
			bool ok = fSession.Lib().Get(load, photo);
			if (ok) {
				EditTarget target;
				GetEditTarget(fSession.Lib(), photo, target);
				ok = fSession.Pics().LoadBase(photo, target.base, image,
					kPreviewEdge, &error) == B_OK && !image.IsEmpty();
			}
			BMessage message(kMsgEditSourceReady);
			message.AddInt64("id", load);
			{
				std::lock_guard<std::mutex> lock(fLock);
				if (serial != fSerial)
					continue;		// editing ended or another photo meanwhile
				if (ok)
					fSource = std::move(image);
			}
			message.AddBool("ok", ok);
			if (!ok) {
				message.AddString("error", error.empty()
					? "The picture cannot be read." : error.c_str());
			}
			fTarget.SendMessage(&message);
			continue;
		}

		Image result;
		{
			// The source is only replaced by this thread or when editing
			// ends: rendering under the lock keeps it alive.
			std::lock_guard<std::mutex> lock(fLock);
			if (fSource.IsEmpty() || serial != fSerial)
				continue;
			if (crop) {
				ApplyGeometry(fSource, params, result, 0, false);
				ApplyAdjustments(result, params);
			} else
				RenderEdit(fSource, params, result);
		}
		BBitmap* bitmap = MakeBitmap(result);
		if (bitmap == nullptr)
			continue;
		int width, height;
		if (crop) {
			photos::UncroppedSize(fEditTarget.baseWidth, fEditTarget.baseHeight,
				params, width, height);
		} else {
			EditedSize(fEditTarget.baseWidth, fEditTarget.baseHeight, params,
				width, height);
		}
		BMessage message(kMsgEditRendered);
		message.AddPointer("bitmap", bitmap);
		message.AddInt32("serial", serial);
		message.AddInt32("width", width);
		message.AddInt32("height", height);
		message.AddBool("crop", crop);
		if (fTarget.SendMessage(&message) != B_OK)
			delete bitmap;
	}
}

}	// namespace ui
