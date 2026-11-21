// People: the persons the user has named and the groups of faces that are
// waiting for a name; and, above a person's photos, the faces that may be
// that person too.
#pragma once

#include "Ui.h"
#include "platform/Session.h"

#include <View.h>
#include <Window.h>

#include <vector>

class BListView;
class BTextControl;

namespace ui {

class PeopleView : public BView {
public:
	explicit PeopleView(photos::Session& session);

	void Reload();
	void TileReady(int64_t faceId);
	int CountPeople() const { return (int)fPeople.size(); }
	int CountGroups() const { return (int)fClusters.size(); }

	virtual void AttachedToWindow();
	virtual void Draw(BRect updateRect);
	virtual void FrameResized(float width, float height);
	virtual void MouseDown(BPoint where);
	virtual void MouseMoved(BPoint where, uint32 transit, const BMessage* drag);

private:
	struct Tile {
		BRect rect;
		bool person;
		int index;
	};

	void _Layout();
	void _UpdateScrollBar();
	int _TileAt(BPoint where) const;

	photos::Session& fSession;
	std::vector<photos::Person> fPeople;
	std::vector<int64_t> fPeopleCovers;
	std::vector<photos::FaceCluster> fClusters;
	std::vector<Tile> fTiles;
	float fPeopleTop;
	float fGroupsTop;
	float fTotalHeight;
	int fTile;
	int fHover;
};

// "Is this also Anna?": faces to confirm or reject.
class SuggestionsView : public BView {
public:
	explicit SuggestionsView(photos::Session& session);

	// Shows the suggestions for a person; hides itself when there are none.
	void SetPerson(int64_t personId);
	void TileReady(int64_t faceId);
	int Count() const { return (int)fSuggestions.size(); }

	virtual void Draw(BRect updateRect);
	virtual void MouseDown(BPoint where);
	virtual BSize MinSize();
	virtual BSize MaxSize();
	virtual BSize PreferredSize();

private:
	photos::Session& fSession;
	int64_t fPersonId;
	BString fName;
	std::vector<photos::FaceSuggestion> fSuggestions;
	std::vector<BRect> fYes;
	std::vector<BRect> fNo;
	std::vector<BRect> fFaces;
};

// Asks for a person's name, offering the names there are.
class NameWindow : public BWindow {
public:
	// `reply` is sent to `target` with "name" added when a name is given.
	NameWindow(BWindow* parent, photos::Session& session, const char* title,
		const char* initial, const BMessage& reply);

	virtual void MessageReceived(BMessage* message);

private:
	void _Filter();

	photos::Session& fSession;
	BMessenger fTarget;
	BMessage fReply;
	BTextControl* fName;
	BListView* fList;
	std::vector<photos::Person> fPeople;
};

}	// namespace ui
