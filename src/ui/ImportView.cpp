#include "ImportView.h"

#include "Theme.h"
#include "platform/ImageIO.h"

#include <Bitmap.h>
#include <LayoutBuilder.h>
#include <ScrollBar.h>
#include <Window.h>

#include <algorithm>

using namespace photos;

namespace ui {

// The scrolling grid of the device's pictures.
class ImportView::Canvas : public BView {
public:
	Canvas(ImportView* owner)
		:
		BView("import canvas", B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
		fOwner(owner),
		fColumns(1),
		fLeft(0),
		fTotalHeight(0),
		fBar(nullptr)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		SetExplicitMinSize(BSize(S(240), S(160)));
		SetExplicitPreferredSize(BSize(S(700), S(500)));
		SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
	}

	void SetScrollBar(BScrollBar* bar) { fBar = bar; }

	void Layout()
	{
		float margin = S(18), gap = S(4);
		int tile = fOwner->TileSize();
		float width = Bounds().Width() - 2 * margin;
		fColumns = std::max(1, (int)((width + gap) / (tile + gap)));
		float used = fColumns * tile + (fColumns - 1) * gap;
		fLeft = margin + std::max(0.0f, floorf((width - used) / 2));
		int rows = ((int)fOwner->Entries().size() + fColumns - 1) / fColumns;
		fTotalHeight = S(14) + rows * (tile + gap) + S(14);
		if (fBar != nullptr) {
			float height = Bounds().Height();
			float maximum = std::max(0.0f, fTotalHeight - height);
			fBar->SetRange(0, maximum);
			fBar->SetProportion(fTotalHeight > 0
				? std::min(1.0f, height / fTotalHeight) : 1);
			fBar->SetSteps(tile + gap, std::max((float)tile + gap, height - tile));
			if (Bounds().top > maximum)
				ScrollTo(BPoint(0, maximum));
		}
	}

	BRect TileRect(int index) const
	{
		float gap = S(4);
		int tile = fOwner->TileSize();
		float left = fLeft + (index % fColumns) * (tile + gap);
		float top = S(14) + (index / fColumns) * (tile + gap);
		return BRect(left, top, left + tile - 1, top + tile - 1);
	}

	virtual void FrameResized(float width, float height)
	{
		Layout();
		Invalidate();
	}

	virtual void AttachedToWindow()
	{
		Layout();
	}

	virtual void Draw(BRect updateRect)
	{
		SetDrawingMode(B_OP_COPY);
		SetHighColor(Theme::Background());
		FillRect(updateRect);
		SetLowColor(Theme::Background());
		std::vector<Entry>& entries = fOwner->Entries();
		BRect bounds = Bounds();
		if (entries.empty()) {
			BFont title(be_bold_font);
			title.SetSize(be_plain_font->Size() * 1.4f);
			SetFont(&title);
			SetHighColor(Theme::SecondaryText());
			// The message may have several lines.
			BString text = fOwner->Message();
			float y = bounds.top + bounds.Height() * 0.4f;
			bool first = true;
			while (text.Length() > 0) {
				int32 end = text.FindFirst('\n');
				BString part = end >= 0 ? BString(text.String(), end) : text;
				BFont font;
				GetFont(&font);
				DrawString(part.String(), BPoint(bounds.left
					+ (bounds.Width() - font.StringWidth(part.String())) / 2, y));
				y += font.Size() * 1.7f;
				if (first) {
					SetFont(be_plain_font);
					SetHighColor(Theme::TertiaryText());
					first = false;
				}
				if (end < 0)
					break;
				text.Remove(0, end + 1);
			}
			return;
		}
		font_height plain;
		be_plain_font->GetHeight(&plain);
		int tile = fOwner->TileSize();
		for (size_t i = 0; i < entries.size(); i++) {
			BRect rect = TileRect((int)i);
			if (rect.top > updateRect.bottom)
				break;
			if (!rect.Intersects(updateRect))
				continue;
			const Entry& entry = entries[i];
			if (entry.thumb != nullptr)
				DrawBitmap(entry.thumb, rect.LeftTop());
			else {
				SetHighColor(Theme::Placeholder());
				FillRect(rect);
				BFont small(be_plain_font);
				small.SetSize(be_plain_font->Size() * 0.85f);
				SetFont(&small);
				SetLowColor(Theme::Placeholder());
				SetHighColor(Theme::TertiaryText());
				DrawTruncated(this, entry.name.c_str(),
					BPoint(rect.left + S(6), rect.bottom - S(8)), tile - S(12));
				SetLowColor(Theme::Background());
			}
			if (!entry.isNew || !entry.importable) {
				// In the library already (or not a picture): held back.
				SetDrawingMode(B_OP_ALPHA);
				rgb_color veil = Theme::Background();
				veil.alpha = 170;
				SetHighColor(veil);
				FillRect(rect);
				SetDrawingMode(B_OP_COPY);
				const char* label = entry.importable ? "Imported" : "Video";
				BFont small(be_plain_font);
				small.SetSize(be_plain_font->Size() * 0.85f);
				SetFont(&small);
				float width = small.StringWidth(label) + S(10);
				BRect tag(rect.left + S(5), rect.top + S(5), rect.left + S(5) + width,
					rect.top + S(5) + plain.ascent + plain.descent + S(2));
				SetHighColor(Theme::Control());
				FillRoundRect(tag, S(4), S(4));
				SetLowColor(Theme::Control());
				SetHighColor(Theme::SecondaryText());
				DrawString(label, BPoint(tag.left + S(5), tag.top + S(1) + plain.ascent));
				SetLowColor(Theme::Background());
			}
			if (entry.selected) {
				SetHighColor(Theme::Accent());
				SetPenSize(S(3));
				StrokeRect(rect.InsetByCopy(S(1), S(1)));
				SetPenSize(1);
				float size = S(20);
				BRect badge(rect.right - size - S(6), rect.top + S(6),
					rect.right - S(6), rect.top + S(6) + size);
				FillEllipse(badge);
				SetHighColor(255, 255, 255);
				SetPenSize(S(2));
				BPoint a(badge.left + size * 0.27f, badge.top + size * 0.52f);
				BPoint b(badge.left + size * 0.44f, badge.top + size * 0.69f);
				BPoint c(badge.left + size * 0.74f, badge.top + size * 0.33f);
				StrokeLine(a, b);
				StrokeLine(b, c);
				SetPenSize(1);
			}
		}
	}

	virtual void MouseDown(BPoint where)
	{
		std::vector<Entry>& entries = fOwner->Entries();
		if (entries.empty()) {
			fOwner->Retry();
			return;
		}
		for (size_t i = 0; i < entries.size(); i++) {
			BRect rect = TileRect((int)i);
			if (rect.top > where.y)
				break;
			if (rect.Contains(where) && entries[i].importable) {
				entries[i].selected = !entries[i].selected;
				Invalidate(rect);
				fOwner->SelectionChanged();
				return;
			}
		}
	}

private:
	ImportView* fOwner;
	int fColumns;
	float fLeft;
	float fTotalHeight;
	BScrollBar* fBar;
};


ImportView::ImportView(Session& session)
	:
	BView("import", 0),
	fSession(session),
	fListing(false),
	fFailed(false),
	fImporting(false),
	fTile((int)S(132)),
	fStop(false),
	fFound(0)
{
	fCanvas = new Canvas(this);
	BScrollBar* bar = new BScrollBar("import scroll", fCanvas, 0, 0, B_VERTICAL);
	fCanvas->SetScrollBar(bar);
	BLayoutBuilder::Group<>(this, B_HORIZONTAL, 0)
		.Add(fCanvas)
		.Add(bar);
	fMessage = "No camera\nConnect a camera, a phone or a memory card.";
}


ImportView::~ImportView()
{
	_Stop();
	_Clear();
}


void
ImportView::_Stop()
{
	fStop = true;
	if (fThread.joinable())
		fThread.join();
	fStop = false;
}


void
ImportView::_Clear()
{
	for (auto& entry : fEntries)
		delete entry.thumb;
	fEntries.clear();
}


BString
ImportView::Title() const
{
	return fName.Length() > 0 ? fName : BString("Import");
}


BString
ImportView::Subtitle() const
{
	if (fListing) {
		BString text("Reading the pictures…");
		int found = fFound;
		if (found > 0)
			text.SetToFormat("Reading the pictures… %d", found);
		return text;
	}
	if (fFailed || fEntries.empty())
		return "";
	int fresh = 0, pictures = 0;
	for (auto& entry : fEntries) {
		if (!entry.importable)
			continue;
		pictures++;
		if (entry.isNew)
			fresh++;
	}
	BString text = CountText(pictures, "picture", "pictures");
	text << ", " << (fresh == 0 ? BString("none") : Number(fresh)) << " new";
	return text;
}


BString
ImportView::StatusText() const
{
	if (fEntries.empty())
		return fListing ? "Reading the device…" : "";
	int selected = 0;
	for (auto& entry : fEntries)
		selected += entry.selected ? 1 : 0;
	BString text;
	if (selected > 0)
		text << Number(selected) << " selected  ·  ";
	text << Subtitle();
	return text;
}


void
ImportView::AddHeaderButtons(HeaderView* header)
{
	if (fEntries.empty() || fListing)
		return;
	int fresh = 0, selected = 0, pictures = 0;
	for (auto& entry : fEntries) {
		if (!entry.importable)
			continue;
		pictures++;
		fresh += entry.isNew ? 1 : 0;
		selected += entry.selected ? 1 : 0;
	}
	uint32 busy = fImporting ? kButtonDisabled : 0;
	if (pictures > fresh) {
		BString all;
		all << "Import all " << Number(pictures);
		header->AddButton(all.String(), kMsgCameraImportAll, busy);
	}
	if (selected > 0) {
		BString label;
		label << "Import " << Number(selected) << " selected";
		header->AddButton(label.String(), kMsgCameraImportSelected, busy);
	}
	BString label;
	if (fresh > 0)
		label << "Import " << Number(fresh) << " new";
	else
		label = "Nothing new";
	header->AddButton(label.String(), kMsgCameraImportNew,
		kButtonAccent | (fresh == 0 ? (uint32)kButtonDisabled : busy));
}


void
ImportView::SelectionChanged()
{
	if (Window() != nullptr)
		Window()->PostMessage(kMsgSelectionChanged);
}


void
ImportView::SelectAll(bool select)
{
	for (auto& entry : fEntries)
		entry.selected = select && entry.importable;
	fCanvas->Invalidate();
	SelectionChanged();
}


void
ImportView::CameraRemoved(const char* key)
{
	if (fKey != key)
		return;
	_Stop();
	_Clear();
#ifdef PHOTOS_HAVE_CAMERAS
	fCamera.reset();
	fItems.clear();
#endif
	fKey.clear();
	fName = "";
	fListing = false;
	fFailed = false;
	fMessage = "The device was disconnected";
	fCanvas->Layout();
	fCanvas->Invalidate();
}


void
ImportView::ImportFinished()
{
	if (!fImporting)
		return;
	fImporting = false;
	// What was imported is not new any more.
	for (auto& entry : fEntries) {
		entry.selected = false;
		entry.isNew = !fSession.Lib().WasImported(
			CameraItemKey(entry.name, entry.size, entry.taken), entry.name, entry.size);
	}
	fCanvas->Invalidate();
	SelectionChanged();
}


#ifdef PHOTOS_HAVE_CAMERAS

void
ImportView::SetCamera(std::shared_ptr<CameraSource> camera)
{
	_Stop();
	_Clear();
	fItems.clear();
	fCamera = camera;
	fKey = camera->Info().key;
	fName = camera->Info().name.c_str();
	fListing = true;
	fFailed = false;
	fImporting = false;
	fFound = 0;
	fMessage = "Reading the pictures…";
	fCanvas->Layout();
	fCanvas->Invalidate();
	fThread = std::thread([this]() { _Run(); });
}


void
ImportView::Retry()
{
	if (fFailed && fCamera && !fListing)
		SetCamera(fCamera);
}


void
ImportView::_Run()
{
	std::shared_ptr<CameraSource> camera = fCamera;
	BMessenger window(Window());
	std::string error;
	status_t status = camera->Open(&error);
	std::vector<CameraItem> items;
	if (status == B_OK) {
		status = camera->ListItems(items, &fStop, [this](int32 found) {
			fFound = found;
		});
		if (status != B_OK)
			error = camera->LastError();
	}
	if (status != B_OK && error.empty())
		error = strerror(status);
	if (fStop)
		return;
	// The newest first, as the library shows them.
	std::sort(items.begin(), items.end(), [](const CameraItem& a, const CameraItem& b) {
		if (a.taken != b.taken)
			return a.taken > b.taken;
		return a.name > b.name;
	});
	{
		std::lock_guard<std::mutex> lock(fListLock);
		fListed = items;
	}
	BMessage listed(kMsgCameraListed);
	listed.AddBool("ok", status == B_OK);
	listed.AddString("error", error.c_str());
	listed.AddString("key", camera->Info().key.c_str());
	window.SendMessage(&listed);
	if (status != B_OK)
		return;

	// Small pictures of them, one after the other (a camera answers one
	// thing at a time).
	int tile = fTile;
	for (size_t i = 0; i < items.size() && !fStop; i++) {
		if (items[i].isVideo)
			continue;
		std::vector<uint8_t> jpeg;
		if (camera->GetThumbnail(items[i], jpeg) != B_OK || jpeg.empty())
			continue;
		Image decoded, covered;
		if (DecodeMemory(jpeg.data(), jpeg.size(), decoded, tile) != B_OK
			|| decoded.IsEmpty())
			continue;
		CoverImage(decoded, tile, tile, covered);
		BBitmap* bitmap = MakeBitmap(covered);
		if (bitmap == nullptr)
			continue;
		BMessage thumb(kMsgCameraThumb);
		thumb.AddInt32("index", (int32)i);
		thumb.AddString("id", items[i].id.c_str());
		thumb.AddPointer("bitmap", bitmap);
		if (fStop || window.SendMessage(&thumb) != B_OK)
			delete bitmap;
	}
	// Done with the device for now (an import opens it again).
	if (!fStop)
		camera->Close();
}


void
ImportView::StartImport(int what, bool deleteAfterwards)
{
	if (!fCamera || fImporting || fListing || fEntries.empty())
		return;
	std::vector<CameraItem> chosen;
	for (size_t i = 0; i < fEntries.size() && i < fItems.size(); i++) {
		const Entry& entry = fEntries[i];
		if (!entry.importable)
			continue;
		bool take = what == 2 || (what == 1 && entry.selected)
			|| (what == 0 && entry.isNew);
		if (take)
			chosen.push_back(fItems[i]);
	}
	if (chosen.empty())
		return;
	// The camera does one thing at a time: the thumbnails stop here.
	_Stop();
	fImporting = true;
	fSession.Imports().ImportFromCamera(fCamera, chosen, deleteAfterwards);
}


void
ImportView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgCameraListed:
		{
			if (fKey != message->GetString("key", ""))
				break;
			fListing = false;
			_Clear();
			if (!message->GetBool("ok", false)) {
				fFailed = true;
				fMessage = "The pictures cannot be read\n";
				fMessage << message->GetString("error", "") << "\nClick to try again.";
			} else {
				fFailed = false;
				{
					std::lock_guard<std::mutex> lock(fListLock);
					fItems = fListed;
				}
				for (auto& item : fItems) {
					Entry entry;
					entry.id = item.id;
					entry.name = item.name;
					entry.size = item.size;
					entry.taken = item.taken;
					entry.importable = !item.isVideo && IsPictureName(item.name);
					entry.isNew = entry.importable && !fSession.Lib().WasImported(
						CameraItemKey(item.name, item.size, item.taken), item.name,
						item.size);
					fEntries.push_back(entry);
				}
				if (fEntries.empty())
					fMessage = "No pictures on this device";
			}
			fCanvas->Layout();
			fCanvas->Invalidate();
			break;
		}

		case kMsgCameraThumb:
		{
			BBitmap* bitmap = nullptr;
			message->FindPointer("bitmap", (void**)&bitmap);
			int32 index = message->GetInt32("index", -1);
			const char* id = message->GetString("id", "");
			if (index >= 0 && index < (int32)fEntries.size()
				&& fEntries[index].id == id && fEntries[index].thumb == nullptr) {
				fEntries[index].thumb = bitmap;
				fCanvas->Invalidate(fCanvas->TileRect(index));
			} else
				delete bitmap;
			break;
		}

		default:
			BView::MessageReceived(message);
	}
}

#else	// no cameras in this build

void
ImportView::Retry()
{
}


void
ImportView::_Run()
{
}


void
ImportView::StartImport(int what, bool deleteAfterwards)
{
}


void
ImportView::MessageReceived(BMessage* message)
{
	BView::MessageReceived(message);
}

#endif

}	// namespace ui
