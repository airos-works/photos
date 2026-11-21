#include "Theme.h"

#include <dlfcn.h>

namespace Theme {

namespace {

bool sDark = false;
rgb_color sDocument;
rgb_color sDocumentText;
rgb_color sPanel;
rgb_color sPanelText;
rgb_color sControl;
rgb_color sHighlight;


rgb_color
Make(int r, int g, int b, int a = 255)
{
	rgb_color color;
	color.red = (uint8)r;
	color.green = (uint8)g;
	color.blue = (uint8)b;
	color.alpha = (uint8)a;
	return color;
}

}	// namespace


float
Luminance(rgb_color c)
{
	return (0.2126f * c.red + 0.7152f * c.green + 0.0722f * c.blue) / 255.0f;
}


rgb_color
Mix(rgb_color a, rgb_color b, float amount)
{
	if (amount < 0)
		amount = 0;
	if (amount > 1)
		amount = 1;
	return Make((int)(a.red + (b.red - a.red) * amount + 0.5f),
		(int)(a.green + (b.green - a.green) * amount + 0.5f),
		(int)(a.blue + (b.blue - a.blue) * amount + 0.5f), 255);
}


rgb_color
WithAlpha(rgb_color color, uint8 alpha)
{
	color.alpha = alpha;
	return color;
}


void
Refresh()
{
	sDocument = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
	sDocumentText = ui_color(B_DOCUMENT_TEXT_COLOR);
	sPanel = ui_color(B_PANEL_BACKGROUND_COLOR);
	sPanelText = ui_color(B_PANEL_TEXT_COLOR);
	sControl = ui_color(B_CONTROL_BACKGROUND_COLOR);
	sHighlight = ui_color(B_CONTROL_HIGHLIGHT_COLOR);

	// air/OS's switch: ui_dark_mode() where the system has it (looked up at
	// run time so the binary also runs on systems without it), otherwise
	// the palette tells.
	typedef bool (*DarkModeFunction)();
	static DarkModeFunction darkMode = (DarkModeFunction)dlsym(RTLD_DEFAULT,
		"_Z12ui_dark_modev");
	bool paletteDark = Luminance(sDocument) < 0.45f;
	sDark = darkMode != nullptr ? (darkMode() || paletteDark) : paletteDark;
	// A dark switch with a light custom document color still reads as the
	// palette says; trust the actual colors for contrast.
	if (darkMode != nullptr && darkMode() && !paletteDark)
		sDark = Luminance(sPanel) < 0.45f;
}


bool
IsDark()
{
	return sDark;
}


rgb_color
Background()
{
	return sDark ? sDocument : Make(255, 255, 255);
}


rgb_color
Sidebar()
{
	return sDark ? Mix(sPanel, Make(0, 0, 0), 0.18f)
		: Mix(sPanel, Make(255, 255, 255), 0.45f);
}


rgb_color
Header()
{
	return sDark ? Mix(sPanel, Make(0, 0, 0), 0.05f)
		: Mix(sPanel, Make(255, 255, 255), 0.7f);
}


rgb_color
Text()
{
	return sDark ? Mix(sDocumentText, Make(255, 255, 255), 0.2f)
		: Make(28, 28, 30);
}


rgb_color
SecondaryText()
{
	return sDark ? Make(170, 170, 176) : Make(110, 110, 115);
}


rgb_color
TertiaryText()
{
	return sDark ? Make(130, 130, 136) : Make(150, 150, 155);
}


rgb_color
Separator()
{
	return sDark ? Mix(sPanel, Make(255, 255, 255), 0.10f)
		: Mix(sPanel, Make(0, 0, 0), 0.14f);
}


rgb_color
Selection()
{
	return sDark ? Mix(Sidebar(), Make(255, 255, 255), 0.14f)
		: Mix(Sidebar(), Make(0, 0, 0), 0.10f);
}


rgb_color
Hover()
{
	return sDark ? Mix(Sidebar(), Make(255, 255, 255), 0.07f)
		: Mix(Sidebar(), Make(0, 0, 0), 0.05f);
}


rgb_color
Accent()
{
	return sDark ? Make(64, 156, 255) : Make(0, 112, 235);
}


rgb_color
AccentText()
{
	return Make(255, 255, 255);
}


rgb_color
Control()
{
	return sDark ? Mix(sPanel, Make(255, 255, 255), 0.09f)
		: Mix(sPanel, Make(0, 0, 0), 0.08f);
}


rgb_color
ControlSelected()
{
	return sDark ? Mix(sPanel, Make(255, 255, 255), 0.26f) : Make(255, 255, 255);
}


rgb_color
Placeholder()
{
	return sDark ? Mix(sDocument, Make(255, 255, 255), 0.08f)
		: Make(236, 236, 240);
}


rgb_color
Viewer()
{
	return sDark ? Mix(sDocument, Make(0, 0, 0), 0.35f) : Make(246, 246, 248);
}


rgb_color
ViewerText()
{
	return Text();
}


rgb_color
Danger()
{
	return sDark ? Make(255, 105, 97) : Make(215, 40, 40);
}


rgb_color
Favorite()
{
	return Make(255, 69, 96);
}

}	// namespace Theme
