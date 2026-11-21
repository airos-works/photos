#include "People.h"

#include "Image.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace photos {

People::People(Library& library)
	:
	fLibrary(library),
	fDimension(0)
{
}


void
People::SetThresholds(const PeopleThresholds& thresholds)
{
	std::lock_guard<std::mutex> lock(fLock);
	fThresholds = thresholds;
}


float
People::_Similarity(const float* a, const float* b) const
{
	// Embeddings are stored with unit length: the dot product is the cosine.
	float sum = 0;
	for (int i = 0; i < fDimension; i++)
		sum += a[i] * b[i];
	return sum;
}


void
People::_Add(const Face& face)
{
	if (face.embedding.empty())
		return;
	if (fDimension == 0)
		fDimension = (int)face.embedding.size();
	if ((int)face.embedding.size() != fDimension)
		return;		// made by another model: rescanned sooner or later
	Entry entry;
	entry.id = face.id;
	entry.photoId = face.photoId;
	entry.personId = face.personId;
	entry.cluster = face.cluster;
	entry.source = face.source;
	entry.quality = face.quality;
	entry.offset = fVectors.size();
	// Normalised once more: similarity must not depend on who stored it.
	double length = 0;
	for (float value : face.embedding)
		length += (double)value * value;
	float scale = length > 0 ? (float)(1.0 / sqrt(length)) : 0;
	for (float value : face.embedding)
		fVectors.push_back(value * scale);
	fIndex[entry.id] = fEntries.size();
	fEntries.push_back(entry);
}


void
People::_Rebuild()
{
	fByPerson.clear();
	fClusters.clear();
	fIndex.clear();
	for (size_t i = 0; i < fEntries.size(); i++) {
		Entry& entry = fEntries[i];
		fIndex[entry.id] = i;
		if (entry.personId != 0)
			fByPerson[entry.personId].push_back(i);
		else if (entry.cluster != 0) {
			Centroid& centroid = fClusters[entry.cluster];
			if (centroid.sum.empty())
				centroid.sum.assign(fDimension, 0);
			const float* vector = _Vector(entry);
			for (int d = 0; d < fDimension; d++)
				centroid.sum[d] += vector[d];
			centroid.count++;
			centroid.photos[entry.photoId]++;
		}
	}
}


void
People::Load()
{
	std::vector<Face> faces;
	fLibrary.ListAllFaces(faces, true);
	std::vector<std::pair<int64_t, int64_t>> rejections;
	fLibrary.ListRejections(rejections);
	std::sort(rejections.begin(), rejections.end());

	std::lock_guard<std::mutex> lock(fLock);
	fEntries.clear();
	fVectors.clear();
	fDimension = 0;
	// The dimension of the newest faces counts (a model change leaves old
	// ones behind until they are rescanned).
	if (!faces.empty())
		fDimension = (int)faces.back().embedding.size();
	for (auto& face : faces)
		_Add(face);
	fRejections = rejections;
	_Rebuild();
}


bool
People::_Rejected(int64_t faceId, int64_t personId) const
{
	return std::binary_search(fRejections.begin(), fRejections.end(),
		std::make_pair(faceId, personId));
}


bool
People::_Sure(float best, float nextBest) const
{
	if (best >= fThresholds.certain)
		return true;
	return best >= fThresholds.same && best - nextBest >= fThresholds.margin;
}


int64_t
People::_BestPerson(const Entry& entry, float& similarity, int64_t onlyPerson,
	float* nextBest) const
{
	const float* vector = _Vector(entry);
	int64_t best = 0;
	similarity = -1;
	float second = -1;
	for (auto& person : fByPerson) {
		// With nextBest asked for, everyone is looked at even when only one
		// person is wanted: who else the face resembles matters.
		if (onlyPerson != 0 && person.first != onlyPerson && nextBest == nullptr)
			continue;
		if (_Rejected(entry.id, person.first))
			continue;
		// A person is as similar as their most similar face; faces the user
		// named count in full, recognised ones a little less so that a
		// chain of look-alikes does not drift away from the person.
		float personBest = -1;
		for (size_t index : person.second) {
			const Entry& other = fEntries[index];
			if (other.id == entry.id)
				continue;
			float value = _Similarity(vector, _Vector(other));
			if (other.source != kFaceUser)
				value -= 0.04f;
			personBest = std::max(personBest, value);
		}
		if (personBest > similarity) {
			second = similarity;
			similarity = personBest;
			best = person.first;
		} else if (personBest > second)
			second = personBest;
	}
	if (nextBest != nullptr)
		*nextBest = second;
	if (onlyPerson != 0 && best != onlyPerson)
		return 0;
	return best;
}


int64_t
People::_PlaceInCluster(Entry& entry)
{
	const float* vector = _Vector(entry);
	// Close to the middle of a group. The bar is "same": lower ones chain
	// different people together (docs/FACES.md).
	float threshold = fThresholds.same;
	int64_t best = 0;
	float bestValue = threshold;
	for (auto& cluster : fClusters) {
		const Centroid& centroid = cluster.second;
		if (centroid.count == 0)
			continue;
		// Nobody is twice in a photo: faces of one photo are different
		// people (or the same pattern mistaken for faces several times).
		if (centroid.photos.count(entry.photoId) != 0)
			continue;
		double dot = 0, length = 0;
		for (int d = 0; d < fDimension; d++) {
			dot += centroid.sum[d] * vector[d];
			length += (double)centroid.sum[d] * centroid.sum[d];
		}
		float value = length > 0 ? (float)(dot / sqrt(length)) : 0;
		if (value > bestValue) {
			bestValue = value;
			best = cluster.first;
		}
	}
	if (best == 0)
		best = entry.id;	// a group of its own, named after its first face
	Centroid& centroid = fClusters[best];
	if (centroid.sum.empty())
		centroid.sum.assign(fDimension, 0);
	for (int d = 0; d < fDimension; d++)
		centroid.sum[d] += vector[d];
	centroid.count++;
	centroid.photos[entry.photoId]++;
	entry.cluster = best;
	return best;
}


void
People::_SetPerson(Entry& entry, int64_t personId, int source)
{
	size_t index = fIndex[entry.id];
	if (entry.personId != 0) {
		auto& list = fByPerson[entry.personId];
		list.erase(std::remove(list.begin(), list.end(), index), list.end());
		if (list.empty())
			fByPerson.erase(entry.personId);
	} else if (entry.cluster != 0) {
		auto found = fClusters.find(entry.cluster);
		if (found != fClusters.end()) {
			const float* vector = _Vector(entry);
			for (int d = 0; d < fDimension; d++)
				found->second.sum[d] -= vector[d];
			auto photo = found->second.photos.find(entry.photoId);
			if (photo != found->second.photos.end() && --photo->second <= 0)
				found->second.photos.erase(photo);
			if (--found->second.count <= 0)
				fClusters.erase(found);
		}
		entry.cluster = 0;
	}
	entry.personId = personId;
	entry.source = personId != 0 ? source : (int)kFaceUnassigned;
	if (personId != 0)
		fByPerson[personId].push_back(index);
}


void
People::Place(const std::vector<Face>& faces)
{
	std::vector<std::pair<int64_t, int64_t>> assignments;	// face -> person
	std::vector<std::pair<int64_t, int64_t>> clusters;		// face -> cluster
	{
		std::lock_guard<std::mutex> lock(fLock);
		// Who is in the photo already (the faces of a call are one photo's).
		std::set<std::pair<int64_t, int64_t>> present;
		for (auto& face : faces) {
			if (face.personId != 0)
				present.insert(std::make_pair(face.photoId, face.personId));
		}
		for (auto& face : faces) {
			if (face.embedding.empty() || fIndex.count(face.id) != 0)
				continue;
			size_t before = fEntries.size();
			_Add(face);
			if (fEntries.size() == before)
				continue;
			Entry& entry = fEntries.back();
			if (entry.personId != 0) {
				fByPerson[entry.personId].push_back(fEntries.size() - 1);
				continue;	// kept from an earlier scan of the photo
			}
			entry.cluster = 0;
			if (entry.quality < fThresholds.minQuality)
				continue;
			float similarity, nextBest;
			int64_t person = _BestPerson(entry, similarity, 0, &nextBest);
			if (person != 0 && _Sure(similarity, nextBest)
				&& present.insert(std::make_pair(entry.photoId, person)).second) {
				entry.personId = person;
				entry.source = kFaceAuto;
				fByPerson[person].push_back(fEntries.size() - 1);
				assignments.emplace_back(entry.id, person);
			} else
				clusters.emplace_back(entry.id, _PlaceInCluster(entry));
		}
	}
	for (auto& assignment : assignments)
		fLibrary.AssignFace(assignment.first, assignment.second, kFaceAuto);
	fLibrary.SetFaceClusters(clusters);
}


void
People::Forget(int64_t photoId)
{
	std::lock_guard<std::mutex> lock(fLock);
	std::vector<Entry> kept;
	std::vector<float> vectors;
	bool changed = false;
	for (auto& entry : fEntries) {
		if (entry.photoId == photoId) {
			changed = true;
			continue;
		}
		Entry copy = entry;
		copy.offset = vectors.size();
		vectors.insert(vectors.end(), fVectors.begin() + entry.offset,
			fVectors.begin() + entry.offset + fDimension);
		kept.push_back(copy);
	}
	if (!changed)
		return;
	fEntries.swap(kept);
	fVectors.swap(vectors);
	_Rebuild();
}


int
People::_RecognizeLocked(int64_t personId)
{
	// Candidates in parallel, then applied in order: each new example can
	// bring more, so repeat until nothing changes (a few rounds at most).
	int total = 0;
	std::vector<std::pair<int64_t, int64_t>> assignments;
	for (int round = 0; round < 4; round++) {
		std::vector<int64_t> found(fEntries.size(), 0);
		ParallelRows((int)fEntries.size(), [&](int first, int end) {
			for (int i = first; i < end; i++) {
				const Entry& entry = fEntries[i];
				if (entry.personId != 0 || entry.quality < fThresholds.minQuality)
					continue;
				float similarity, nextBest;
				int64_t person = _BestPerson(entry, similarity, personId, &nextBest);
				if (person != 0 && _Sure(similarity, nextBest))
					found[i] = person;
			}
		}, 256);
		// A person is in a photo once: the first face found stands.
		std::set<std::pair<int64_t, int64_t>> present;
		for (auto& entry : fEntries) {
			if (entry.personId != 0)
				present.insert(std::make_pair(entry.photoId, entry.personId));
		}
		int count = 0;
		for (size_t i = 0; i < fEntries.size(); i++) {
			if (found[i] == 0)
				continue;
			if (!present.insert(std::make_pair(fEntries[i].photoId, found[i])).second)
				continue;
			_SetPerson(fEntries[i], found[i], kFaceAuto);
			assignments.emplace_back(fEntries[i].id, found[i]);
			count++;
		}
		total += count;
		if (count == 0)
			break;
	}
	// Written to the library per person.
	std::map<int64_t, std::vector<int64_t>> byPerson;
	for (auto& assignment : assignments)
		byPerson[assignment.second].push_back(assignment.first);
	for (auto& person : byPerson)
		fLibrary.AssignFaces(person.second, person.first, kFaceAuto);
	return total;
}


int
People::Recognize(int64_t personId)
{
	std::lock_guard<std::mutex> lock(fLock);
	return _RecognizeLocked(personId);
}


int64_t
People::NameCluster(int64_t cluster, const std::string& name)
{
	std::string trimmed = Trim(name);
	if (trimmed.empty())
		return 0;
	int64_t person = fLibrary.FindPerson(trimmed);
	if (person == 0)
		person = fLibrary.CreatePerson(trimmed);
	if (person == 0)
		return 0;
	std::lock_guard<std::mutex> lock(fLock);
	std::vector<int64_t> faces;
	for (auto& entry : fEntries) {
		if (entry.personId == 0 && entry.cluster == cluster)
			faces.push_back(entry.id);
	}
	for (int64_t id : faces)
		_SetPerson(fEntries[fIndex[id]], person, kFaceUser);
	fLibrary.AssignFaces(faces, person, kFaceUser);
	_RecognizeLocked(person);
	return person;
}


int64_t
People::NameFace(int64_t faceId, const std::string& name)
{
	std::string trimmed = Trim(name);
	if (trimmed.empty())
		return 0;
	int64_t person = fLibrary.FindPerson(trimmed);
	if (person == 0)
		person = fLibrary.CreatePerson(trimmed);
	if (person != 0)
		ConfirmFace(faceId, person);
	return person;
}


void
People::ConfirmFace(int64_t faceId, int64_t personId)
{
	std::lock_guard<std::mutex> lock(fLock);
	auto found = fIndex.find(faceId);
	if (found != fIndex.end())
		_SetPerson(fEntries[found->second], personId, kFaceUser);
	auto rejection = std::make_pair(faceId, personId);
	fRejections.erase(std::remove(fRejections.begin(), fRejections.end(), rejection),
		fRejections.end());
	fLibrary.AssignFace(faceId, personId, kFaceUser);
	if (found != fIndex.end())
		_RecognizeLocked(personId);
}


void
People::RejectFace(int64_t faceId, int64_t personId)
{
	std::lock_guard<std::mutex> lock(fLock);
	auto rejection = std::make_pair(faceId, personId);
	if (!_Rejected(faceId, personId)) {
		fRejections.push_back(rejection);
		std::sort(fRejections.begin(), fRejections.end());
	}
	fLibrary.RejectFace(faceId, personId);
	auto found = fIndex.find(faceId);
	if (found == fIndex.end())
		return;
	Entry& entry = fEntries[found->second];
	if (entry.personId == personId) {
		_SetPerson(entry, 0, kFaceUnassigned);
		if (entry.quality >= fThresholds.minQuality) {
			int64_t cluster = _PlaceInCluster(entry);
			fLibrary.SetFaceCluster(faceId, cluster);
		}
	}
}


void
People::UnassignFace(int64_t faceId)
{
	std::lock_guard<std::mutex> lock(fLock);
	fLibrary.AssignFace(faceId, 0, kFaceUnassigned);
	auto found = fIndex.find(faceId);
	if (found == fIndex.end())
		return;
	Entry& entry = fEntries[found->second];
	_SetPerson(entry, 0, kFaceUnassigned);
	if (entry.quality >= fThresholds.minQuality)
		fLibrary.SetFaceCluster(faceId, _PlaceInCluster(entry));
}


void
People::MergePeople(int64_t from, int64_t into)
{
	if (from == into)
		return;
	std::lock_guard<std::mutex> lock(fLock);
	fLibrary.MergePeople(from, into);
	for (auto& entry : fEntries) {
		if (entry.personId == from)
			entry.personId = into;
	}
	for (auto& rejection : fRejections) {
		if (rejection.second == from)
			rejection.second = into;
	}
	std::sort(fRejections.begin(), fRejections.end());
	_Rebuild();
}


void
People::DeletePerson(int64_t personId)
{
	std::vector<std::pair<int64_t, int64_t>> clusters;
	{
		std::lock_guard<std::mutex> lock(fLock);
		fLibrary.DeletePerson(personId);
		std::vector<size_t> indexes;
		auto found = fByPerson.find(personId);
		if (found != fByPerson.end())
			indexes = found->second;
		for (size_t index : indexes) {
			Entry& entry = fEntries[index];
			_SetPerson(entry, 0, kFaceUnassigned);
			if (entry.quality >= fThresholds.minQuality)
				clusters.emplace_back(entry.id, _PlaceInCluster(entry));
		}
		fRejections.erase(std::remove_if(fRejections.begin(), fRejections.end(),
			[&](const std::pair<int64_t, int64_t>& pair) {
				return pair.second == personId;
			}), fRejections.end());
	}
	fLibrary.SetFaceClusters(clusters);
}


void
People::ListClusters(std::vector<FaceCluster>& clusters, int minPhotos)
{
	clusters.clear();
	std::lock_guard<std::mutex> lock(fLock);
	std::map<int64_t, FaceCluster> byId;
	std::map<int64_t, std::set<int64_t>> photos;
	std::map<int64_t, float> bestQuality;
	for (auto& entry : fEntries) {
		if (entry.personId != 0 || entry.cluster == 0)
			continue;
		FaceCluster& cluster = byId[entry.cluster];
		cluster.id = entry.cluster;
		cluster.faces.push_back(entry.id);
		photos[entry.cluster].insert(entry.photoId);
		auto best = bestQuality.find(entry.cluster);
		if (best == bestQuality.end() || entry.quality > best->second) {
			bestQuality[entry.cluster] = entry.quality;
			cluster.cover = entry.id;
		}
	}
	for (auto& pair : byId) {
		pair.second.photoCount = (int)photos[pair.first].size();
		if (pair.second.photoCount < minPhotos)
			continue;
		clusters.push_back(std::move(pair.second));
	}
	std::sort(clusters.begin(), clusters.end(),
		[](const FaceCluster& a, const FaceCluster& b) {
			if (a.photoCount != b.photoCount)
				return a.photoCount > b.photoCount;
			return a.id < b.id;
		});
}


void
People::Suggestions(int64_t personId, std::vector<FaceSuggestion>& suggestions,
	int limit)
{
	suggestions.clear();
	std::lock_guard<std::mutex> lock(fLock);
	if (fByPerson.find(personId) == fByPerson.end())
		return;
	std::vector<float> values(fEntries.size(), -1);
	ParallelRows((int)fEntries.size(), [&](int first, int end) {
		for (int i = first; i < end; i++) {
			const Entry& entry = fEntries[i];
			if (entry.personId != 0 || entry.quality < fThresholds.minQuality)
				continue;
			float similarity;
			if (_BestPerson(entry, similarity, personId) == personId)
				values[i] = similarity;
		}
	}, 256);
	for (size_t i = 0; i < fEntries.size(); i++) {
		if (values[i] < fThresholds.maybe)
			continue;
		FaceSuggestion suggestion;
		suggestion.faceId = fEntries[i].id;
		suggestion.photoId = fEntries[i].photoId;
		suggestion.similarity = values[i];
		suggestions.push_back(suggestion);
	}
	std::sort(suggestions.begin(), suggestions.end(),
		[](const FaceSuggestion& a, const FaceSuggestion& b) {
			return a.similarity > b.similarity;
		});
	if ((int)suggestions.size() > limit)
		suggestions.resize(limit);
}


int64_t
People::BestFace(int64_t personId)
{
	std::lock_guard<std::mutex> lock(fLock);
	auto found = fByPerson.find(personId);
	if (found == fByPerson.end())
		return 0;
	int64_t best = 0;
	float bestQuality = -1;
	for (size_t index : found->second) {
		const Entry& entry = fEntries[index];
		// A face the user named is a safer picture of the person.
		float quality = entry.quality + (entry.source == kFaceUser ? 0.25f : 0);
		if (quality > bestQuality) {
			bestQuality = quality;
			best = entry.id;
		}
	}
	return best;
}


int
People::CountFaces()
{
	std::lock_guard<std::mutex> lock(fLock);
	return (int)fEntries.size();
}

}	// namespace photos
