#include "InfoView.h"

#include "Theme.h"

#include <Bitmap.h>
#include <Window.h>

#include <cmath>

using namespace photos;

namespace ui {

InfoView::InfoView(Session& session)
	:
	BView("info", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
	fSession(session),
	fPhotoId(0),
	fHover(-1)
{
	SetViewColor(B_TRANSPARENT_COLOR);
	SetEventMask(0);
}


BSize
InfoView::MinSize()
{
	return BSize(S(270), S(200));
}


BSize
InfoView::MaxSize()
{
	return BSize(S(270), B_SIZE_UNLIMITED);
}


BSize
InfoView::PreferredSize()
{
	return BSize(S(270), S(600));
}


void
InfoView::SetPhoto(int64_t id)
{
	fPhotoId = id;
	fVersions.clear();
	fFaces.clear();
	fFaceNames.clear();
	fPhoto = Photo();
	if (id != 0 && fSession.Lib().Get(id, fPhoto)) {
		fSession.Lib().ListVersions(id, fVersions);
		fSession.Lib().ListFaces(id, fFaces);
		for (auto& face : fFaces) {
			Person person;
			BString name;
			if (face.personId != 0 && fSession.Lib().GetPerson(face.personId, person))
				name = person.name.c_str();
			fFaceNames.push_back(name);
		}
	} else
		fPhotoId = 0;
	Invalidate();
}


void
InfoView::TileReady(int64_t faceId)
{
	for (auto& face : fFaces) {
		if (face.id == faceId) {
			Invalidate();
			return;
		}
	}
}


float
InfoView::_Draw(bool measure)
{
	BRect bounds = Bounds();
	float left = S(16), right = bounds.right - S(16);
	float width = right - left;
	float y = S(14);
	font_height plain;
	be_plain_font->GetHeight(&plain);
	float line = ceilf((plain.ascent + plain.descent) * 1.45f);
	fRows.clear();

	auto heading = [&](const char* text) {
		y += S(6);
		if (!measure) {
			BFont font(be_bold_font);
			font.SetSize(be_plain_font->Size() * 0.85f);
			SetFont(&font);
			SetHighColor(Theme::TertiaryText());
			BString upper(text);
			upper.ToUpper();
			DrawString(upper.String(), BPoint(left, y + plain.ascent));
		}
		y += line + S(2);
	};
	auto text = [&](const BString& value, rgb_color color) {
		if (value.Length() == 0)
			return;
		if (!measure) {
			SetFont(be_plain_font);
			SetHighColor(color);
			DrawTruncated(this, value.String(), BPoint(left, y + plain.ascent), width);
		}
		y += line;
	};

	if (fPhotoId == 0)
		return y;

	// Name and date.
	if (!measure) {
		BFont font(be_bold_font);
		font.SetSize(be_plain_font->Size() * 1.1f);
		SetFont(&font);
		SetHighColor(Theme::Text());
		DrawTruncated(this, fPhoto.name.c_str(), BPoint(left, y + plain.ascent), width);
	}
	y += line + S(2);
	text(DateTimeText(fPhoto.taken), Theme::SecondaryText());

	heading("Picture");
	BString size;
	if (fPhoto.width > 0) {
		size.SetToFormat("%d × %d", fPhoto.width, fPhoto.height);
		double megapixels = (double)fPhoto.width * fPhoto.height / 1e6;
		BString mp;
		mp.SetToFormat(megapixels >= 10 ? "  ·  %.0f MP" : "  ·  %.1f MP", megapixels);
		size << mp;
	}
	text(size, Theme::Text());
	BString file = KindName(fPhoto.kind);
	if (fPhoto.size > 0)
		file << "  ·  " << FormatSize(fPhoto.size).c_str();
	text(file, Theme::Text());

	if (!fPhoto.make.empty() || !fPhoto.model.empty() || fPhoto.iso > 0) {
		heading("Camera");
		BString camera = fPhoto.model.c_str();
		// "FUJIFILM X100T": the make unless the model says it already.
		if (!fPhoto.make.empty()
			&& camera.IFindFirst(fPhoto.make.substr(0, 4).c_str()) < 0) {
			camera.Prepend(" ");
			camera.Prepend(fPhoto.make.c_str());
		}
		text(camera, Theme::Text());
		text(fPhoto.lens.c_str(), Theme::SecondaryText());
		BString exposure;
		auto add = [&](const std::string& part) {
			if (part.empty())
				return;
			if (exposure.Length() > 0)
				exposure << "  ·  ";
			exposure << part.c_str();
		};
		add(FormatExposure(fPhoto.exposureTime));
		add(FormatAperture(fPhoto.fNumber));
		if (fPhoto.iso > 0) {
			char iso[32];
			snprintf(iso, sizeof(iso), "ISO %.0f", fPhoto.iso);
			add(iso);
		}
		if (fPhoto.focalLength > 0) {
			char focal[32];
			snprintf(focal, sizeof(focal), fPhoto.focalLength < 10 ? "%.1f mm"
				: "%.0f mm", fPhoto.focalLength);
			add(focal);
		}
		text(exposure, Theme::SecondaryText());
	}

	if (fPhoto.hasGps) {
		heading("Place");
		BString place;
		place.SetToFormat("%.5f° %s, %.5f° %s", fabs(fPhoto.latitude),
			fPhoto.latitude >= 0 ? "N" : "S", fabs(fPhoto.longitude),
			fPhoto.longitude >= 0 ? "E" : "W");
		text(place, Theme::Text());
	}

	heading("Storage");
	if (fPhoto.path.empty()) {
		text("On Immich", Theme::Text());
		Row row;
		row.rect = BRect(left, y, right, y + line);
		row.what = kMsgDownloadOriginal;
		fRows.push_back(row);
		text("Download the original", Theme::Accent());
	} else if (!fPhoto.immichId.empty())
		text("On this computer and on Immich", Theme::Text());
	else if (fPhoto.immichState == kImmichExcluded)
		text("On this computer (not sent to Immich)", Theme::Text());
	else
		text("On this computer", Theme::Text());

	// Versions: the original and what was made of it.
	if (!fVersions.empty()) {
		heading("Versions");
		for (int i = -1; i < (int)fVersions.size(); i++) {
			int64_t id = i < 0 ? 0 : fVersions[i].id;
			bool current = fPhoto.version == id;
			BString label;
			if (i < 0)
				label = "Original";
			else {
				const Version& version = fVersions[i];
				label = version.kind == kVersionEdit ? "Edited"
					: version.source.empty() ? "Saved" : version.source.c_str();
				int64_t local = version.created + LocalOffsetSeconds(version.created);
				CivilTime time = SecondsToCivil(local);
				BString when;
				when.SetToFormat("  ·  %d %s, %d:%02d", time.day,
					MonthName(time.month).String(), time.hour, time.minute);
				label << when;
			}
			Row row;
			row.rect = BRect(left - S(6), y, right + S(6), y + line + S(4));
			row.what = kMsgShowVersion;
			row.value = id;
			if (i >= 0) {
				row.closeRect = BRect(right - S(14), y + S(2), right + S(4),
					y + line + S(2));
			}
			if (!measure) {
				if (current) {
					SetHighColor(Theme::Selection());
					FillRoundRect(row.rect, S(6), S(6));
				} else if ((int)fRows.size() == fHover) {
					SetHighColor(Theme::Hover());
					FillRoundRect(row.rect, S(6), S(6));
				}
				SetFont(current ? be_bold_font : be_plain_font);
				SetHighColor(Theme::Text());
				DrawTruncated(this, label.String(),
					BPoint(left + S(2), y + S(2) + plain.ascent), width - S(24));
				if (i >= 0) {
					SetHighColor(Theme::TertiaryText());
					SetFont(be_plain_font);
					DrawString("×", BPoint(right - S(8), y + S(2) + plain.ascent));
				}
			}
			fRows.push_back(row);
			y += line + S(6);
		}
	}

	if (!fFaces.empty()) {
		heading("People");
		float tile = S(40);
		for (size_t i = 0; i < fFaces.size(); i++) {
			Row row;
			row.rect = BRect(left - S(6), y, right + S(6), y + tile + S(6));
			row.what = kMsgNameFace;
			row.value = fFaces[i].id;
			if (!measure) {
				if ((int)fRows.size() == fHover) {
					SetHighColor(Theme::Hover());
					FillRoundRect(row.rect, S(6), S(6));
				}
				BRect picture(left, y + S(3), left + tile - 1, y + S(3) + tile - 1);
				std::shared_ptr<BBitmap> bitmap = fSession.Pics().FaceTile(
					fFaces[i].id, (int)tile);
				if (bitmap)
					DrawBitmap(bitmap.get(), picture.LeftTop());
				else {
					SetHighColor(Theme::Placeholder());
					FillRect(picture);
				}
				bool named = fFaceNames[i].Length() > 0;
				SetFont(be_plain_font);
				SetHighColor(named ? Theme::Text() : Theme::Accent());
				DrawTruncated(this, named ? fFaceNames[i].String() : "Add a name…",
					BPoint(left + tile + S(10), y + S(3) + (tile + plain.ascent) / 2
						- plain.descent),
					width - tile - S(10));
			}
			fRows.push_back(row);
			y += tile + S(8);
		}
	}
	return y + S(12);
}


void
InfoView::Draw(BRect updateRect)
{
	SetDrawingMode(B_OP_COPY);
	SetHighColor(Theme::Sidebar());
	FillRect(updateRect);
	SetLowColor(Theme::Sidebar());
	SetHighColor(Theme::Separator());
	StrokeLine(Bounds().LeftTop(), Bounds().LeftBottom());
	_Draw(false);
}


void
InfoView::MouseDown(BPoint where)
{
	for (auto& row : fRows) {
		if (!row.rect.Contains(where))
			continue;
		BMessage message(row.what);
		if (row.what == kMsgShowVersion) {
			if (row.closeRect.IsValid() && row.closeRect.Contains(where))
				message.what = kMsgDeleteVersion;
			message.AddInt64("version", row.value);
		} else if (row.what == kMsgNameFace)
			message.AddInt64("face", row.value);
		Window()->PostMessage(&message);
		return;
	}
}


void
InfoView::MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
{
	int hover = -1;
	if (transit != B_EXITED_VIEW) {
		for (size_t i = 0; i < fRows.size(); i++) {
			if (fRows[i].rect.Contains(where))
				hover = (int)i;
		}
	}
	if (hover != fHover) {
		fHover = hover;
		Invalidate();
	}
}

}	// namespace ui
