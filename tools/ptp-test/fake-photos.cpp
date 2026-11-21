// A stand-in for Photos in the agent's test: an application with Photos'
// signature that writes down the camera messages it gets.
//
//   fake_photos LOGFILE       (started by the roster: /tmp/fake-photos.log)
#include <Application.h>

#include <cstdio>
#include <ctime>

static const char* sLog = "/tmp/fake-photos.log";


class FakePhotos : public BApplication {
public:
	FakePhotos()
		:
		BApplication("application/x-vnd.airos-Photos")
	{
	}

	virtual void ReadyToRun()
	{
		_Log("started");
	}

	virtual void ArgvReceived(int32 argc, char** argv)
	{
		if (argc > 1)
			sLog = argv[1];
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == 'camA' || message->what == 'camR') {
			char line[512];
			snprintf(line, sizeof(line), "%s key=%s name=\"%s\" kind=%d "
				"launch=%d initial=%d",
				message->what == 'camA' ? "arrived" : "removed",
				message->GetString("key", "?"),
				message->GetString("name", "?"),
				(int)message->GetInt32("kind", -1),
				(int)message->GetBool("launch", false),
				(int)message->GetBool("initial", false));
			_Log(line);
			return;
		}
		BApplication::MessageReceived(message);
	}

private:
	void _Log(const char* text)
	{
		FILE* file = fopen(sLog, "a");
		if (file == NULL)
			return;
		fprintf(file, "%ld %s\n", (long)time(NULL), text);
		fclose(file);
	}
};


int
main()
{
	FakePhotos app;
	app.Run();
	return 0;
}
