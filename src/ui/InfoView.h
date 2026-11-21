// What is known about the photo shown: when and with what it was taken,
// where its original is, its versions, the people in it.
#pragma once

#include "Ui.h"
#include "platform/Session.h"

#include <View.h>

#include <vector>

namespace ui {

class InfoView : public BView {
public:
	explicit InfoView(photos::Session& session);

	void SetPhoto(int64_t id);
	void TileReady(int64_t faceId);

	virtual void Draw(BRect updateRect);
	virtual void MouseDown(BPoint where);
	virtual void MouseMoved(BPoint where, uint32 transit, const BMessage* drag);
	virtual BSize MinSize();
	virtual BSize MaxSize();
	virtual BSize PreferredSize();

private:
	struct Row {
		BRect rect;
		uint32 what = 0;
		int64_t value = 0;
		BRect closeRect;		// versions: the delete mark
	};

	float _Draw(bool measure);

	photos::Session& fSession;
	int64_t fPhotoId;
	photos::Photo fPhoto;
	std::vector<photos::Version> fVersions;
	std::vector<photos::Face> fFaces;
	std::vector<BString> fFaceNames;
	std::vector<Row> fRows;
	int fHover;
};

}	// namespace ui
