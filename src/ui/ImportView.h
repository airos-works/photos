// A camera's, phone's or memory card's pictures, to choose what to import:
// everything that is new, a selection, or all of it.
#pragma once

#include "Chrome.h"
#include "platform/Session.h"

#include <View.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class BBitmap;
class BScrollBar;

namespace ui {

class ImportView : public BView {
public:
	explicit ImportView(photos::Session& session);
	virtual ~ImportView();

	BString Title() const;
	BString Subtitle() const;
	BString StatusText() const;
	void AddHeaderButtons(HeaderView* header);
	const std::string& CameraKey() const { return fKey; }

#ifdef PHOTOS_HAVE_CAMERAS
	// Opens the camera and lists its pictures on a thread.
	void SetCamera(std::shared_ptr<photos::CameraSource> camera);
#endif
	void CameraRemoved(const char* key);
	// what: 0 the new pictures, 1 the selected ones, 2 all.
	void StartImport(int what, bool deleteAfterwards);
	void ImportFinished();
	void SelectAll(bool select);

	virtual void MessageReceived(BMessage* message);

	// For the canvas inside.
	struct Entry {
		std::string id;
		std::string name;
		int64_t size = 0;
		int64_t taken = 0;
		bool isNew = true;
		bool selected = false;
		bool importable = true;
		BBitmap* thumb = nullptr;
	};
	std::vector<Entry>& Entries() { return fEntries; }
	int TileSize() const { return fTile; }
	const BString& Message() const { return fMessage; }
	bool IsListing() const { return fListing; }
	void SelectionChanged();
	void Retry();

private:
	class Canvas;

	void _Stop();
	void _Run();
	void _Clear();

	photos::Session& fSession;
	Canvas* fCanvas;
	std::string fKey;
	BString fName;
	BString fMessage;			// instead of pictures: connecting, an error
	bool fListing;
	bool fFailed;
	bool fImporting;
	int fTile;
	std::vector<Entry> fEntries;
	std::thread fThread;
	std::atomic<bool> fStop;
	std::atomic<int32> fFound;
#ifdef PHOTOS_HAVE_CAMERAS
	std::shared_ptr<photos::CameraSource> fCamera;
	std::mutex fListLock;
	std::vector<photos::CameraItem> fListed;	// from the thread, under fListLock
	std::vector<photos::CameraItem> fItems;		// what fEntries were made of
#endif
};

}	// namespace ui
