// Who is who: face embeddings grouped into people.
//
// A face the scanner found is compared with the faces of the people the user
// has named. One that is clearly a known person is assigned to them; the rest
// are grouped with unnamed faces that look alike, so that the user names a
// group once ("who is this?") instead of every face. Each name, confirmation
// ("yes, that is her") and rejection adds examples: that is how Photos learns
// a face.
#pragma once

#include "Library.h"

#include <map>
#include <mutex>
#include <vector>

namespace photos {

struct PeopleThresholds {
	// Cosine similarity of two embeddings (FaceMath.h has the tuned values).
	float same = 0.50f;			// surely the same person: assign
	float maybe = 0.36f;		// possibly: suggest, group
	float minQuality = 0.30f;	// faces below are not grouped or recognised
};

struct FaceCluster {
	int64_t id = 0;
	int64_t cover = 0;			// the best face to show
	std::vector<int64_t> faces;
	int photoCount = 0;
};

struct FaceSuggestion {
	int64_t faceId = 0;
	int64_t photoId = 0;
	float similarity = 0;
};

class People {
public:
	explicit People(Library& library);

	void SetThresholds(const PeopleThresholds& thresholds);
	// Reads all faces from the library (at start, and after it changed
	// behind this object's back).
	void Load();

	// Faces just stored for a photo (with their ids and embeddings): assigns
	// or groups them.
	void Place(const std::vector<Face>& faces);
	// The photo's faces were replaced or the photo deleted.
	void Forget(int64_t photoId);

	// Names every face of a group; the person is made if the name is new.
	// Returns the person.
	int64_t NameCluster(int64_t cluster, const std::string& name);
	int64_t NameFace(int64_t faceId, const std::string& name);
	void ConfirmFace(int64_t faceId, int64_t personId);
	void RejectFace(int64_t faceId, int64_t personId);
	void UnassignFace(int64_t faceId);
	void MergePeople(int64_t from, int64_t into);
	void DeletePerson(int64_t personId);

	// Looks again for faces of this person (0: of everyone) among the
	// unassigned ones. Returns how many were assigned.
	int Recognize(int64_t personId = 0);

	// Groups of unnamed faces, largest first.
	void ListClusters(std::vector<FaceCluster>& clusters, int minFaces = 1);
	// Unassigned faces that may be this person, most similar first.
	void Suggestions(int64_t personId, std::vector<FaceSuggestion>& suggestions,
		int limit = 60);
	// The best-looking face of a person, for its picture.
	int64_t BestFace(int64_t personId);
	int CountFaces();

private:
	struct Entry {
		int64_t id;
		int64_t photoId;
		int64_t personId;
		int64_t cluster;
		int source;
		float quality;
		size_t offset;		// into fVectors
	};
	struct Centroid {
		std::vector<float> sum;
		int count = 0;
	};

	const float* _Vector(const Entry& entry) const { return fVectors.data() + entry.offset; }
	float _Similarity(const float* a, const float* b) const;
	void _Add(const Face& face);
	void _Rebuild();
	bool _Rejected(int64_t faceId, int64_t personId) const;
	// The best person for a vector and the similarity to them.
	int64_t _BestPerson(const Entry& entry, float& similarity,
		int64_t onlyPerson = 0) const;
	int64_t _PlaceInCluster(Entry& entry);
	void _SetPerson(Entry& entry, int64_t personId, int source);
	int _RecognizeLocked(int64_t personId);

	Library& fLibrary;
	std::mutex fLock;
	PeopleThresholds fThresholds;
	int fDimension;
	std::vector<Entry> fEntries;
	std::vector<float> fVectors;
	std::map<int64_t, size_t> fIndex;				// face id -> entry
	std::map<int64_t, std::vector<size_t>> fByPerson;
	std::map<int64_t, Centroid> fClusters;
	std::vector<std::pair<int64_t, int64_t>> fRejections;	// sorted
};

}	// namespace photos
