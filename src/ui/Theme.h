// Photos' colors, derived from the system's UI colors so the app follows
// air/OS's light/dark switch (and any custom palette). Views call
// Theme::Refresh() on B_COLORS_UPDATED and redraw.
#pragma once

#include <GraphicsDefs.h>
#include <InterfaceDefs.h>

namespace Theme {

void Refresh();
bool IsDark();

rgb_color Background();		// the grid's canvas
rgb_color Sidebar();
rgb_color Header();			// toolbar and status bar
rgb_color Text();
rgb_color SecondaryText();
rgb_color TertiaryText();
rgb_color Separator();
rgb_color Selection();		// selected sidebar row, pressed button
rgb_color Hover();
rgb_color Accent();			// selection ring, favourites, links
rgb_color AccentText();		// text on Accent
rgb_color Control();		// segmented control, slider track
rgb_color ControlSelected();
rgb_color Placeholder();	// a tile whose picture is not there yet
rgb_color Viewer();			// behind a single photo
rgb_color ViewerText();
rgb_color Danger();			// destructive actions
rgb_color Favorite();

rgb_color Mix(rgb_color a, rgb_color b, float amount);	// amount of b
rgb_color WithAlpha(rgb_color color, uint8 alpha);
float Luminance(rgb_color color);

}	// namespace Theme
