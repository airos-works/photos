// photos_agent: runs in the background from login (resources/
// photos_agent.launch) and opens Photos when a camera, a phone or a memory
// card with pictures is connected.
//
// What was connected already when the agent started is left alone: only an
// arrival opens Photos. Photos, when it runs, watches for cameras itself, so
// the agent does nothing then. "open_on_connect false" in
// ~/config/settings/Photos/agent turns the agent's one job off: it then does
// not watch for cameras at all (no USB or volume watching threads), only for
// a change of that file, which takes effect at once either way.
#include "platform/CameraSource.h"

#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Node.h>
#include <NodeMonitor.h>
#include <Path.h>
#include <Roster.h>

#include <strings.h>

#include <cstdio>
#include <cstring>

using namespace photos;

static const char* kAgentSignature = "application/x-vnd.airos-Photos-Agent";
static const char* kPhotosSignature = "application/x-vnd.airos-Photos";


class Agent : public BApplication {
public:
	Agent()
		:
		BApplication(kAgentSignature),
		fWatcher(BMessenger(this)),
		fWatching(false)
	{
	}

	virtual void ReadyToRun()
	{
		_WatchSettings();
		_Update();
	}

	virtual bool QuitRequested()
	{
		stop_watching(be_app_messenger);
		fWatcher.Stop();
		return true;
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == B_NODE_MONITOR) {
			// the settings file was written, replaced or removed
			_WatchSettings();
			_Update();
			return;
		}
		if (message->what != kMsgCameraArrived) {
			BApplication::MessageReceived(message);
			return;
		}
		if (message->GetBool("initial", false) || !_OpenOnConnect()
			|| be_roster->IsRunning(kPhotosSignature)) {
			return;
		}
		BMessage arrived(kMsgCameraArrived);
		arrived.AddString("key", message->GetString("key", ""));
		arrived.AddString("name", message->GetString("name", ""));
		arrived.AddInt32("kind", message->GetInt32("kind", 0));
		// Photos was started for this camera (its own watcher reports the
		// same one as "initial").
		arrived.AddBool("launch", true);
		be_roster->Launch(kPhotosSignature, &arrived);
	}

private:
	// Watches for cameras only while they are to open Photos. A camera that
	// is there when watching starts counts as connected already.
	void _Update()
	{
		bool open = _OpenOnConnect();
		if (open && !fWatching)
			fWatching = fWatcher.Start() == B_OK;
		else if (!open && fWatching) {
			fWatcher.Stop();
			fWatching = false;
		}
	}

	// The settings directory (the file created, replaced or removed) and
	// the file itself (written in place). Without a directory there is no
	// file, and the agent watches for cameras; an arrival reads the file
	// again anyway.
	void _WatchSettings()
	{
		stop_watching(be_app_messenger);
		BPath path;
		if (_SettingsPath(path) != B_OK)
			return;
		node_ref ref;
		BEntry file(path.Path());
		if (file.GetNodeRef(&ref) == B_OK)
			watch_node(&ref, B_WATCH_STAT, be_app_messenger);
		BEntry directory;
		if (file.GetParent(&directory) == B_OK
			&& directory.GetNodeRef(&ref) == B_OK) {
			watch_node(&ref, B_WATCH_DIRECTORY, be_app_messenger);
		}
	}

	static status_t _SettingsPath(BPath& path)
	{
		status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
		if (status == B_OK)
			status = path.Append("Photos/agent");
		return status;
	}

	// Read again at every arrival, too.
	static bool _OpenOnConnect()
	{
		BPath path;
		if (_SettingsPath(path) != B_OK)
			return true;
		FILE* file = fopen(path.Path(), "r");
		if (file == NULL)
			return true;
		bool open = true;
		char line[256];
		while (fgets(line, sizeof(line), file) != NULL) {
			char name[64], value[64];
			if (line[0] == '#' || sscanf(line, "%63s %63s", name, value) != 2)
				continue;
			if (strcmp(name, "open_on_connect") == 0) {
				open = !(strcasecmp(value, "false") == 0
					|| strcasecmp(value, "off") == 0
					|| strcasecmp(value, "no") == 0
					|| strcmp(value, "0") == 0);
			}
		}
		fclose(file);
		return open;
	}

	CameraWatcher	fWatcher;
	bool			fWatching;
};


int
main()
{
	Agent agent;
	agent.Run();
	return 0;
}
