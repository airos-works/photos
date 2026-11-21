// One photo, large: fitted to the view, zoomed with the wheel or a double
// click, dragged around when it is larger than the view. In the editor it
// shows the edited preview and, for cropping, the frame to drag.
#pragma once

#include "Ui.h"
#include "platform/Session.h"

#include <View.h>

#include <memory>
#include <vector>

class BBitmap;

namespace ui {

class ViewerView : public BView {
public:
	explicit ViewerView(photos::Session& session);
	virtual ~ViewerView();

	// Shows a photo: at once what is at hand (its tile), then the picture
	// asked from the session.
	void ShowPhoto(int64_t id, int tileSize);
	int64_t PhotoId() const { return fPhotoId; }
	// The photo changed (edited, another version): load it again.
	void Reload();
	void Clear();
	// From kMsgViewPicture.
	void PictureArrived(BMessage* message);
	bool IsRemotePreview() const { return fRemote; }

	void ZoomToFit();
	void ZoomToActual();
	void ZoomBy(float factor);
	bool IsZoomed() const { return !fFit; }

	void SetShowFaces(bool show);
	bool ShowsFaces() const { return fShowFaces; }
	void FacesChanged();

	// The editor's preview replaces the photo's picture while it is set.
	// naturalWidth/Height: the size the edited picture will have.
	void SetEditPicture(BBitmap* bitmap, int naturalWidth, int naturalHeight);
	void EndEdit();
	// The crop frame, in fractions of the picture shown. aspect: width /
	// height to keep, 0 for any.
	void SetCropMode(bool on, float x, float y, float w, float h, float aspect);
	void SetCropAspect(float aspect);
	void GetCrop(float& x, float& y, float& w, float& h) const;
	bool InCropMode() const { return fCropMode; }

	virtual void Draw(BRect updateRect);
	virtual void FrameResized(float width, float height);
	virtual void MouseDown(BPoint where);
	virtual void MouseMoved(BPoint where, uint32 transit, const BMessage* drag);
	virtual void MouseUp(BPoint where);
	virtual void KeyDown(const char* bytes, int32 numBytes);
	virtual void MessageReceived(BMessage* message);

private:
	enum CropHandle {
		kNone = 0, kMove,
		kLeft = 1 << 4, kRight = 1 << 5, kTop = 1 << 6, kBottom = 1 << 7
	};

	BBitmap* _Shown() const;
	void _NaturalSize(float& width, float& height) const;
	float _FitZoom() const;
	BRect _PictureRect() const;
	void _ClampPan();
	void _Request(bool full);
	BRect _CropRect() const;
	int _CropHandleAt(BPoint where) const;
	void _DragCrop(BPoint where);
	void _DrawFaces(BRect picture);
	void _DrawCrop(BRect picture);
	int _FaceAt(BPoint where) const;

	photos::Session& fSession;
	int64_t fPhotoId;
	int64_t fRevision;
	int32 fToken;
	BBitmap* fBitmap;
	std::shared_ptr<BBitmap> fStandIn;	// the grid's tile, until the picture comes
	BBitmap* fEditBitmap;
	int fNaturalWidth;
	int fNaturalHeight;
	int fEditWidth;
	int fEditHeight;
	bool fFinal;
	bool fFull;
	bool fRemote;
	bool fFullRequested;
	BString fError;

	bool fFit;
	float fZoom;			// screen pixels per picture pixel
	BPoint fPan;			// offset of the picture's centre from the view's
	bool fDragging;
	BPoint fDragStart;
	BPoint fPanStart;

	bool fShowFaces;
	std::vector<photos::Face> fFaces;
	std::vector<BString> fFaceNames;

	bool fCropMode;
	float fCropX, fCropY, fCropW, fCropH;
	float fCropAspect;
	int fCropHandle;
	BPoint fCropGrab;
	float fCropStart[4];
};

}	// namespace ui
