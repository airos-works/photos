#include "ViewerView.h"

#include "Theme.h"

#include <Bitmap.h>
#include <Cursor.h>
#include <Window.h>

#include <algorithm>
#include <cmath>

using namespace photos;

namespace ui {

ViewerView::ViewerView(Session& session)
	:
	BView("viewer", B_WILL_DRAW | B_FRAME_EVENTS | B_NAVIGABLE
		| B_FULL_UPDATE_ON_RESIZE),
	fSession(session),
	fPhotoId(0),
	fRevision(0),
	fToken(0),
	fBitmap(nullptr),
	fEditBitmap(nullptr),
	fNaturalWidth(0),
	fNaturalHeight(0),
	fEditWidth(0),
	fEditHeight(0),
	fFinal(false),
	fFull(false),
	fRemote(false),
	fFullRequested(false),
	fFit(true),
	fZoom(1),
	fDragging(false),
	fShowFaces(false),
	fCropMode(false),
	fCropX(0), fCropY(0), fCropW(1), fCropH(1),
	fCropAspect(0),
	fCropHandle(kNone)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


ViewerView::~ViewerView()
{
	delete fBitmap;
	delete fEditBitmap;
}


void
ViewerView::Clear()
{
	delete fBitmap;
	fBitmap = nullptr;
	delete fEditBitmap;
	fEditBitmap = nullptr;
	fStandIn.reset();
	fPhotoId = 0;
	fFaces.clear();
	fFaceNames.clear();
	fError = "";
	fCropMode = false;
	fToken++;
	Invalidate();
}


void
ViewerView::_Request(bool full)
{
	BRect bounds = Bounds();
	// Twice the view: sharp when fitted, and some room to zoom.
	int edge = full ? 0 : (int)(std::max(bounds.Width(), bounds.Height()) * 2);
	if (!full)
		edge = std::max(edge, 1600);
	fToken++;
	fSession.Pics().RequestView(fPhotoId, edge, BMessenger(this), fToken);
}


void
ViewerView::ShowPhoto(int64_t id, int tileSize)
{
	Photo photo;
	if (!fSession.Lib().Get(id, photo)) {
		Clear();
		return;
	}
	delete fBitmap;
	fBitmap = nullptr;
	delete fEditBitmap;
	fEditBitmap = nullptr;
	fCropMode = false;
	fPhotoId = id;
	fRevision = photo.revision;
	fError = "";
	fFinal = fFull = fRemote = fFullRequested = false;
	fNaturalWidth = photo.width;
	fNaturalHeight = photo.height;
	if (photo.version != 0) {
		Version version;
		if (fSession.Lib().GetVersion(photo.version, version) && version.width > 0) {
			fNaturalWidth = version.width;
			fNaturalHeight = version.height;
		}
	}
	fStandIn = tileSize > 0 ? fSession.Pics().Tile(id, photo.revision, tileSize)
		: nullptr;
	fFit = true;
	fPan = BPoint(0, 0);
	FacesChanged();
	_Request(false);
	Invalidate();
}


void
ViewerView::Reload()
{
	if (fPhotoId == 0)
		return;
	bool faces = fShowFaces;
	int64_t id = fPhotoId;
	// Keep showing the old picture until the new one is there.
	BBitmap* old = fBitmap;
	fBitmap = nullptr;
	ShowPhoto(id, 0);
	if (fPhotoId == id && fBitmap == nullptr)
		fBitmap = old;
	else
		delete old;
	fShowFaces = faces;
}


void
ViewerView::PictureArrived(BMessage* message)
{
	BBitmap* bitmap = nullptr;
	message->FindPointer("bitmap", (void**)&bitmap);
	if (message->GetInt32("token", -1) != fToken) {
		delete bitmap;
		return;
	}
	const char* error = message->GetString("error", nullptr);
	if (bitmap == nullptr) {
		if (error != nullptr && fBitmap == nullptr)
			fError = error;
		Invalidate();
		return;
	}
	delete fBitmap;
	fBitmap = bitmap;
	fFinal = message->GetBool("final", true);
	fFull = message->GetBool("full", false);
	fRemote = message->GetBool("remote", false);
	fError = "";
	if (fNaturalWidth <= 0 || fNaturalHeight <= 0 || fFull) {
		if (fFull || fNaturalWidth <= 0) {
			fNaturalWidth = bitmap->Bounds().IntegerWidth() + 1;
			fNaturalHeight = bitmap->Bounds().IntegerHeight() + 1;
		}
	} else {
		// The database may have the size the other way round (a file whose
		// orientation it could not read).
		float bitmapAspect = (bitmap->Bounds().Width() + 1) / (bitmap->Bounds().Height() + 1);
		float naturalAspect = (float)fNaturalWidth / fNaturalHeight;
		if ((bitmapAspect > 1.05f && naturalAspect < 0.95f)
			|| (bitmapAspect < 0.95f && naturalAspect > 1.05f))
			std::swap(fNaturalWidth, fNaturalHeight);
	}
	fStandIn.reset();
	_ClampPan();
	Invalidate();
	Window()->PostMessage(kMsgSelectionChanged);
}


BBitmap*
ViewerView::_Shown() const
{
	if (fEditBitmap != nullptr)
		return fEditBitmap;
	if (fBitmap != nullptr)
		return fBitmap;
	return fStandIn.get();
}


void
ViewerView::_NaturalSize(float& width, float& height) const
{
	if (fEditBitmap != nullptr && fEditWidth > 0) {
		width = fEditWidth;
		height = fEditHeight;
		return;
	}
	if (fNaturalWidth > 0 && fNaturalHeight > 0) {
		// A stand-in tile is square: it is shown as a square.
		if (fBitmap == nullptr && fStandIn) {
			width = height = (float)std::min(fNaturalWidth, fNaturalHeight);
			return;
		}
		width = fNaturalWidth;
		height = fNaturalHeight;
		return;
	}
	BBitmap* bitmap = _Shown();
	if (bitmap != nullptr) {
		width = bitmap->Bounds().Width() + 1;
		height = bitmap->Bounds().Height() + 1;
	} else
		width = height = 1;
}


float
ViewerView::_FitZoom() const
{
	float width, height;
	_NaturalSize(width, height);
	BRect bounds = Bounds();
	float margin = fCropMode ? S(28) : S(10);
	float zoom = std::min((bounds.Width() - 2 * margin) / width,
		(bounds.Height() - 2 * margin) / height);
	// Small pictures are not blown up (but the editor's preview is: its
	// natural size is the full picture's).
	return std::max(0.01f, std::min(zoom, 1.0f));
}


BRect
ViewerView::_PictureRect() const
{
	float width, height;
	_NaturalSize(width, height);
	float zoom = fFit ? _FitZoom() : fZoom;
	BRect bounds = Bounds();
	float w = floorf(width * zoom), h = floorf(height * zoom);
	float left = floorf(bounds.left + (bounds.Width() - w) / 2 + (fFit ? 0 : fPan.x));
	float top = floorf(bounds.top + (bounds.Height() - h) / 2 + (fFit ? 0 : fPan.y));
	return BRect(left, top, left + w - 1, top + h - 1);
}


void
ViewerView::_ClampPan()
{
	if (fFit) {
		fPan = BPoint(0, 0);
		return;
	}
	float width, height;
	_NaturalSize(width, height);
	BRect bounds = Bounds();
	float overX = std::max(0.0f, (width * fZoom - bounds.Width()) / 2);
	float overY = std::max(0.0f, (height * fZoom - bounds.Height()) / 2);
	fPan.x = std::max(-overX, std::min(overX, fPan.x));
	fPan.y = std::max(-overY, std::min(overY, fPan.y));
}


void
ViewerView::ZoomToFit()
{
	fFit = true;
	fPan = BPoint(0, 0);
	Invalidate();
}


void
ViewerView::ZoomToActual()
{
	if (fCropMode)
		return;
	fFit = false;
	fZoom = 1.0f;
	_ClampPan();
	if (!fFull && !fFullRequested && fEditBitmap == nullptr && !fRemote) {
		fFullRequested = true;
		_Request(true);
	}
	Invalidate();
}


void
ViewerView::ZoomBy(float factor)
{
	if (fCropMode)
		return;
	float fit = _FitZoom();
	float zoom = (fFit ? fit : fZoom) * factor;
	if (zoom <= fit * 1.02f) {
		ZoomToFit();
		return;
	}
	fFit = false;
	fZoom = std::min(zoom, 8.0f);
	fPan.x *= factor;
	fPan.y *= factor;
	_ClampPan();
	// More than the picture at hand can show sharply: the full one.
	BBitmap* bitmap = _Shown();
	float width, height;
	_NaturalSize(width, height);
	if (bitmap != nullptr && !fFull && !fFullRequested && fEditBitmap == nullptr
		&& !fRemote && width * fZoom > (bitmap->Bounds().Width() + 1) * 1.15f) {
		fFullRequested = true;
		_Request(true);
	}
	Invalidate();
}


void
ViewerView::SetShowFaces(bool show)
{
	fShowFaces = show;
	Invalidate();
}


void
ViewerView::FacesChanged()
{
	fFaces.clear();
	fFaceNames.clear();
	if (fPhotoId == 0)
		return;
	fSession.Lib().ListFaces(fPhotoId, fFaces);
	for (auto& face : fFaces) {
		Person person;
		BString name;
		if (face.personId != 0 && fSession.Lib().GetPerson(face.personId, person))
			name = person.name.c_str();
		fFaceNames.push_back(name);
	}
	if (fShowFaces)
		Invalidate();
}


void
ViewerView::SetEditPicture(BBitmap* bitmap, int naturalWidth, int naturalHeight)
{
	delete fEditBitmap;
	fEditBitmap = bitmap;
	fEditWidth = naturalWidth;
	fEditHeight = naturalHeight;
	_ClampPan();
	Invalidate();
}


void
ViewerView::EndEdit()
{
	delete fEditBitmap;
	fEditBitmap = nullptr;
	fCropMode = false;
	fFit = true;
	Invalidate();
}


void
ViewerView::SetCropMode(bool on, float x, float y, float w, float h, float aspect)
{
	fCropMode = on;
	fCropX = x;
	fCropY = y;
	fCropW = w;
	fCropH = h;
	fCropAspect = aspect;
	fCropHandle = kNone;
	if (on)
		fFit = true;
	Invalidate();
}


void
ViewerView::SetCropAspect(float aspect)
{
	fCropAspect = aspect;
	if (aspect <= 0 || !fCropMode)
		return;
	// The largest frame of that shape inside the current one, same centre.
	float width, height;
	_NaturalSize(width, height);
	float pixelW = fCropW * width, pixelH = fCropH * height;
	float centerX = fCropX + fCropW / 2, centerY = fCropY + fCropH / 2;
	if (pixelW / pixelH > aspect)
		pixelW = pixelH * aspect;
	else
		pixelH = pixelW / aspect;
	// As large as fits around that centre.
	float grow = std::min(std::min(centerX, 1 - centerX) * 2 * width / pixelW,
		std::min(centerY, 1 - centerY) * 2 * height / pixelH);
	float area = std::max(fCropW * fCropH, 0.0001f);
	float wanted = sqrtf(area * width * height / (pixelW * pixelH));
	grow = std::min(grow, std::max(1.0f, wanted));
	pixelW *= grow;
	pixelH *= grow;
	fCropW = pixelW / width;
	fCropH = pixelH / height;
	fCropX = centerX - fCropW / 2;
	fCropY = centerY - fCropH / 2;
	Invalidate();
	Window()->PostMessage(kMsgCropChanged);
}


void
ViewerView::GetCrop(float& x, float& y, float& w, float& h) const
{
	x = fCropX;
	y = fCropY;
	w = fCropW;
	h = fCropH;
}


BRect
ViewerView::_CropRect() const
{
	BRect picture = _PictureRect();
	float w = picture.Width() + 1, h = picture.Height() + 1;
	return BRect(picture.left + fCropX * w, picture.top + fCropY * h,
		picture.left + (fCropX + fCropW) * w, picture.top + (fCropY + fCropH) * h);
}


int
ViewerView::_CropHandleAt(BPoint where) const
{
	BRect crop = _CropRect();
	float reach = S(12);
	if (!crop.InsetByCopy(-reach, -reach).Contains(where))
		return kNone;
	int handle = 0;
	if (fabsf(where.x - crop.left) <= reach)
		handle |= kLeft;
	else if (fabsf(where.x - crop.right) <= reach)
		handle |= kRight;
	if (fabsf(where.y - crop.top) <= reach)
		handle |= kTop;
	else if (fabsf(where.y - crop.bottom) <= reach)
		handle |= kBottom;
	return handle != 0 ? handle : (int)kMove;
}


void
ViewerView::_DragCrop(BPoint where)
{
	BRect picture = _PictureRect();
	float w = picture.Width() + 1, h = picture.Height() + 1;
	float dx = (where.x - fCropGrab.x) / w;
	float dy = (where.y - fCropGrab.y) / h;
	float left = fCropStart[0], top = fCropStart[1];
	float right = left + fCropStart[2], bottom = top + fCropStart[3];
	const float kMinimum = 0.04f;
	if (fCropHandle == kMove) {
		float width = right - left, height = bottom - top;
		left = std::max(0.0f, std::min(1 - width, left + dx));
		top = std::max(0.0f, std::min(1 - height, top + dy));
		right = left + width;
		bottom = top + height;
	} else {
		if (fCropHandle & kLeft)
			left = std::max(0.0f, std::min(right - kMinimum, left + dx));
		if (fCropHandle & kRight)
			right = std::min(1.0f, std::max(left + kMinimum, right + dx));
		if (fCropHandle & kTop)
			top = std::max(0.0f, std::min(bottom - kMinimum, top + dy));
		if (fCropHandle & kBottom)
			bottom = std::min(1.0f, std::max(top + kMinimum, bottom + dy));
		if (fCropAspect > 0) {
			// Keep the shape: the dragged side leads, the other follows
			// around the opposite corner or the middle.
			float aspect = fCropAspect * h / w;		// in fractions
			float width = right - left, height = bottom - top;
			bool horizontal = (fCropHandle & (kLeft | kRight)) != 0;
			bool vertical = (fCropHandle & (kTop | kBottom)) != 0;
			if (horizontal && !vertical) {
				height = width / aspect;
				float middle = (fCropStart[1] * 2 + fCropStart[3]) / 2;
				top = middle - height / 2;
				bottom = top + height;
			} else if (vertical && !horizontal) {
				width = height * aspect;
				float middle = (fCropStart[0] * 2 + fCropStart[2]) / 2;
				left = middle - width / 2;
				right = left + width;
			} else {
				if (width / aspect > height)
					height = width / aspect;
				else
					width = height * aspect;
				if (fCropHandle & kLeft)
					left = right - width;
				else
					right = left + width;
				if (fCropHandle & kTop)
					top = bottom - height;
				else
					bottom = top + height;
			}
			// Back inside the picture, keeping the shape.
			float scale = 1;
			if (right - left > 1)
				scale = std::min(scale, 1 / (right - left));
			if (bottom - top > 1)
				scale = std::min(scale, 1 / (bottom - top));
			if (left < 0)
				scale = std::min(scale, right / (right - left));
			if (right > 1)
				scale = std::min(scale, (1 - left) / (right - left));
			if (top < 0)
				scale = std::min(scale, bottom / (bottom - top));
			if (bottom > 1)
				scale = std::min(scale, (1 - top) / (bottom - top));
			if (scale < 1) {
				float newWidth = (right - left) * scale;
				float newHeight = (bottom - top) * scale;
				if (fCropHandle & kLeft)
					left = right - newWidth;
				else if (fCropHandle & kRight)
					right = left + newWidth;
				else {
					float middle = (left + right) / 2;
					left = middle - newWidth / 2;
					right = left + newWidth;
				}
				if (fCropHandle & kTop)
					top = bottom - newHeight;
				else if (fCropHandle & kBottom)
					bottom = top + newHeight;
				else {
					float middle = (top + bottom) / 2;
					top = middle - newHeight / 2;
					bottom = top + newHeight;
				}
			}
			left = std::max(0.0f, left);
			top = std::max(0.0f, top);
			right = std::min(1.0f, right);
			bottom = std::min(1.0f, bottom);
		}
	}
	fCropX = left;
	fCropY = top;
	fCropW = right - left;
	fCropH = bottom - top;
	Invalidate();
}


void
ViewerView::_DrawCrop(BRect picture)
{
	BRect crop = _CropRect();
	SetDrawingMode(B_OP_ALPHA);
	rgb_color shade = Theme::Viewer();
	shade.alpha = 175;
	SetHighColor(shade);
	// The four bands around the frame.
	FillRect(BRect(picture.left, picture.top, picture.right, crop.top - 1));
	FillRect(BRect(picture.left, crop.bottom + 1, picture.right, picture.bottom));
	FillRect(BRect(picture.left, crop.top, crop.left - 1, crop.bottom));
	FillRect(BRect(crop.right + 1, crop.top, picture.right, crop.bottom));
	// Thirds.
	SetHighColor(255, 255, 255, 110);
	for (int i = 1; i < 3; i++) {
		float x = crop.left + crop.Width() * i / 3;
		float y = crop.top + crop.Height() * i / 3;
		StrokeLine(BPoint(x, crop.top), BPoint(x, crop.bottom));
		StrokeLine(BPoint(crop.left, y), BPoint(crop.right, y));
	}
	SetDrawingMode(B_OP_COPY);
	SetHighColor(255, 255, 255);
	StrokeRect(crop);
	SetHighColor(0, 0, 0);
	StrokeRect(crop.InsetByCopy(-1, -1));
	// Corner brackets.
	SetHighColor(255, 255, 255);
	float arm = S(18), thick = S(3);
	for (int corner = 0; corner < 4; corner++) {
		bool right = corner & 1, bottom = corner & 2;
		float x = right ? crop.right : crop.left;
		float y = bottom ? crop.bottom : crop.top;
		float sx = right ? -1 : 1, sy = bottom ? -1 : 1;
		BRect horizontal(std::min(x, x + sx * arm), std::min(y, y + sy * thick),
			std::max(x, x + sx * arm), std::max(y, y + sy * thick));
		BRect vertical(std::min(x, x + sx * thick), std::min(y, y + sy * arm),
			std::max(x, x + sx * thick), std::max(y, y + sy * arm));
		FillRect(horizontal);
		FillRect(vertical);
	}
}


void
ViewerView::_DrawFaces(BRect picture)
{
	float w = picture.Width() + 1, h = picture.Height() + 1;
	BFont font(be_plain_font);
	font.SetSize(be_plain_font->Size() * 0.95f);
	SetFont(&font);
	font_height height;
	font.GetHeight(&height);
	for (size_t i = 0; i < fFaces.size(); i++) {
		const Face& face = fFaces[i];
		BRect box(picture.left + face.x * w, picture.top + face.y * h,
			picture.left + (face.x + face.width) * w,
			picture.top + (face.y + face.height) * h);
		SetDrawingMode(B_OP_ALPHA);
		SetPenSize(S(2));
		SetHighColor(0, 0, 0, 110);
		StrokeRoundRect(box.InsetByCopy(-1, -1), S(8), S(8));
		SetHighColor(255, 255, 255, 235);
		StrokeRoundRect(box, S(8), S(8));
		SetPenSize(1);
		BString label = fFaceNames[i].Length() > 0 ? fFaceNames[i] : BString("Who is this?");
		float labelWidth = font.StringWidth(label.String()) + S(14);
		float labelHeight = height.ascent + height.descent + S(6);
		BRect tag(box.left + (box.Width() - labelWidth) / 2, box.bottom + S(5),
			box.left + (box.Width() + labelWidth) / 2, box.bottom + S(5) + labelHeight);
		SetHighColor(0, 0, 0, 165);
		FillRoundRect(tag, labelHeight / 2, labelHeight / 2);
		SetHighColor(fFaceNames[i].Length() > 0 ? rgb_color{255, 255, 255, 255}
			: rgb_color{160, 205, 255, 255});
		DrawString(label.String(), BPoint(tag.left + S(7),
			tag.top + S(3) + height.ascent));
		SetDrawingMode(B_OP_COPY);
	}
}


int
ViewerView::_FaceAt(BPoint where) const
{
	if (!fShowFaces)
		return -1;
	BRect picture = _PictureRect();
	float w = picture.Width() + 1, h = picture.Height() + 1;
	for (size_t i = 0; i < fFaces.size(); i++) {
		const Face& face = fFaces[i];
		BRect box(picture.left + face.x * w, picture.top + face.y * h,
			picture.left + (face.x + face.width) * w,
			picture.top + (face.y + face.height) * h + S(30));
		if (box.Contains(where))
			return (int)i;
	}
	return -1;
}


void
ViewerView::Draw(BRect updateRect)
{
	SetDrawingMode(B_OP_COPY);
	SetHighColor(Theme::Viewer());
	FillRect(updateRect);
	BBitmap* bitmap = _Shown();
	BRect bounds = Bounds();
	if (bitmap == nullptr) {
		SetFont(be_plain_font);
		SetLowColor(Theme::Viewer());
		SetHighColor(Theme::SecondaryText());
		const char* text = fError.Length() > 0 ? fError.String()
			: fPhotoId != 0 ? "Loading…" : "";
		DrawString(text, BPoint(bounds.left
			+ (bounds.Width() - be_plain_font->StringWidth(text)) / 2,
			bounds.top + bounds.Height() / 2));
		return;
	}
	BRect picture = _PictureRect();
	BRect source = bitmap->Bounds();
	if (bitmap == fStandIn.get() && fBitmap == nullptr && fEditBitmap == nullptr) {
		// The square tile stands in the middle of where the photo will be.
		DrawBitmap(bitmap, source, picture, B_FILTER_BITMAP_BILINEAR);
	} else
		DrawBitmap(bitmap, source, picture, B_FILTER_BITMAP_BILINEAR);
	if (fCropMode)
		_DrawCrop(picture);
	else if (fShowFaces && fEditBitmap == nullptr)
		_DrawFaces(picture);

	if (fRemote && fEditBitmap == nullptr) {
		// The picture is the server's preview: say so, quietly.
		const char* note = "Preview from Immich · the original is on the server";
		SetFont(be_plain_font);
		float width = be_plain_font->StringWidth(note) + S(20);
		font_height height;
		be_plain_font->GetHeight(&height);
		float tall = height.ascent + height.descent + S(8);
		BRect tag(bounds.left + (bounds.Width() - width) / 2, bounds.bottom - tall - S(12),
			bounds.left + (bounds.Width() + width) / 2, bounds.bottom - S(12));
		SetDrawingMode(B_OP_ALPHA);
		SetHighColor(0, 0, 0, 150);
		FillRoundRect(tag, tall / 2, tall / 2);
		SetHighColor(255, 255, 255, 255);
		DrawString(note, BPoint(tag.left + S(10), tag.top + S(4) + height.ascent));
		SetDrawingMode(B_OP_COPY);
	}
}


void
ViewerView::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	_ClampPan();
	Invalidate();
}


void
ViewerView::MouseDown(BPoint where)
{
	MakeFocus(true);
	BMessage* message = Window()->CurrentMessage();
	int32 clicks = message->GetInt32("clicks", 1);
	if (fCropMode) {
		fCropHandle = _CropHandleAt(where);
		if (fCropHandle != kNone) {
			fCropGrab = where;
			fCropStart[0] = fCropX;
			fCropStart[1] = fCropY;
			fCropStart[2] = fCropW;
			fCropStart[3] = fCropH;
			SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
		}
		return;
	}
	int face = _FaceAt(where);
	if (face >= 0 && fEditBitmap == nullptr) {
		BMessage name(kMsgNameFace);
		name.AddInt64("face", fFaces[face].id);
		Window()->PostMessage(&name);
		return;
	}
	if (clicks >= 2) {
		if (fFit)
			ZoomToActual();
		else
			ZoomToFit();
		return;
	}
	if (!fFit) {
		fDragging = true;
		fDragStart = where;
		fPanStart = fPan;
		SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
	}
}


void
ViewerView::MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
{
	if (fCropMode) {
		if (fCropHandle != kNone)
			_DragCrop(where);
		return;
	}
	if (fDragging) {
		fPan = fPanStart + (where - fDragStart);
		_ClampPan();
		Invalidate();
	}
}


void
ViewerView::MouseUp(BPoint where)
{
	if (fCropMode && fCropHandle != kNone) {
		fCropHandle = kNone;
		Window()->PostMessage(kMsgCropChanged);
	}
	fDragging = false;
}


void
ViewerView::KeyDown(const char* bytes, int32 numBytes)
{
	switch (bytes[0]) {
		case B_LEFT_ARROW:
		case B_UP_ARROW:
			if (fEditBitmap == nullptr && !fCropMode)
				Window()->PostMessage(kMsgPrevious);
			return;
		case B_RIGHT_ARROW:
		case B_DOWN_ARROW:
		case B_SPACE:
			if (fEditBitmap == nullptr && !fCropMode)
				Window()->PostMessage(kMsgNext);
			return;
		case B_ESCAPE:
			Window()->PostMessage(fEditBitmap != nullptr || fCropMode
				? kMsgEditCancel : kMsgCloseViewer);
			return;
		case B_DELETE:
		case B_BACKSPACE:
			if (fEditBitmap == nullptr && !fCropMode)
				Window()->PostMessage(kMsgMoveToTrash);
			return;
		case B_ENTER:
			if (fEditBitmap != nullptr || fCropMode)
				Window()->PostMessage(kMsgEditDone);
			return;
		case '+':
		case '=':
			ZoomBy(1.25f);
			return;
		case '-':
			ZoomBy(0.8f);
			return;
		case '0':
			ZoomToFit();
			return;
		case '1':
			ZoomToActual();
			return;
	}
	BView::KeyDown(bytes, numBytes);
}


void
ViewerView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case photos::kMsgViewPicture:
			PictureArrived(message);
			break;

		case B_MOUSE_WHEEL_CHANGED:
		{
			float delta = message->GetFloat("be:wheel_delta_y", 0);
			if (delta != 0)
				ZoomBy(delta < 0 ? 1.15f : 1 / 1.15f);
			break;
		}

		default:
			if (message->WasDropped()) {
				Window()->PostMessage(message);
				break;
			}
			BView::MessageReceived(message);
	}
}

}	// namespace ui
