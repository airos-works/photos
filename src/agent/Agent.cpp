// photos_agent: runs in the background from login (resources/
// photos_agent.launch) and opens Photos when a camera, a phone or a memory
// card with pictures is connected.
//
// What was connected already when the agent started is left alone: only an
// arrival opens Photos. Photos, when it runs, watches for cameras itself, so
// the agent does nothing then. "open_on_connect false" in
// ~/config/settings/Photos/agent turns the agent's one job off.
#include "platform/CameraSource.h"

#include <Application.h>
#include <FindDirectory.h>
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
		fWatcher(BMessenger(this))
	{
	}

	virtual void ReadyToRun()
	{
		fWatcher.Start();
	}

	virtual bool QuitRequested()
	{
		fWatcher.Stop();
		return true;
	}

	virtual void MessageReceived(BMessage* message)
	{
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
	// Read at every arrival: a change takes effect at once.
	static bool _OpenOnConnect()
	{
		BPath path;
		if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK
			|| path.Append("Photos/agent") != B_OK) {
			return true;
		}
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
};


int
main()
{
	Agent agent;
	agent.Run();
	return 0;
}
