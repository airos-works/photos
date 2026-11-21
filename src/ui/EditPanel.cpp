#include "EditPanel.h"

#include "Theme.h"

#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <SeparatorView.h>
#include <Slider.h>
#include <StringView.h>
#include <Window.h>

#include <cmath>

namespace ui {

namespace {

const uint32 kMsgSlider = 'epSl';
const uint32 kMsgAspect = 'epAs';
const uint32 kMsgCropToggle = 'epCr';
const uint32 kMsgTurnLeft = 'epTl';
const uint32 kMsgTurnRight = 'epTr';
const uint32 kMsgFlip = 'epFl';

struct Aspect {
	const char* label;
	float value;
};

const Aspect kAspects[] = {
	{"Free", 0}, {"Original", -1}, {"Square", 1}, {"4 : 3", 4.0f / 3},
	{"3 : 2", 1.5f}, {"16 : 9", 16.0f / 9}, {"3 : 4", 0.75f}, {"2 : 3", 2.0f / 3},
	{"9 : 16", 9.0f / 16}
};


BStringView*
Heading(const char* text)
{
	BStringView* view = new BStringView("heading", text);
	BFont font(be_bold_font);
	view->SetFont(&font);
	return view;
}

}	// namespace


EditPanel::EditPanel()
	:
	BView("edit panel", B_WILL_DRAW),
	fAspect(0),
	fRotation(0),
	fFlip(false)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	fCrop = new BCheckBox("crop", "Crop", new BMessage(kMsgCropToggle));
	BPopUpMenu* menu = new BPopUpMenu("aspect");
	for (size_t i = 0; i < sizeof(kAspects) / sizeof(kAspects[0]); i++) {
		BMessage* message = new BMessage(kMsgAspect);
		message->AddFloat("aspect", kAspects[i].value);
		BMenuItem* item = new BMenuItem(kAspects[i].label, message);
		if (i == 0)
			item->SetMarked(true);
		menu->AddItem(item);
	}
	fAspectField = new BMenuField("aspect", "Shape:", menu);
	fAspectField->SetEnabled(false);

	BButton* left = new BButton("left", "Turn left", new BMessage(kMsgTurnLeft));
	BButton* right = new BButton("right", "Turn right", new BMessage(kMsgTurnRight));
	BButton* flip = new BButton("flip", "Flip", new BMessage(kMsgFlip));

	fStraighten = _Slider("Straighten", -450, 450);
	fExposure = _Slider("Exposure", -200, 200);
	fContrast = _Slider("Contrast", -100, 100);
	fHighlights = _Slider("Highlights", -100, 100);
	fShadows = _Slider("Shadows", -100, 100);
	fSaturation = _Slider("Saturation", -100, 100);
	fWarmth = _Slider("Warmth", -100, 100);
	fTint = _Slider("Tint", -100, 100);

	fEnhance = new BButton("enhance", "Enhance", new BMessage(kMsgEditEnhance));
	fReset = new BButton("reset", "Reset", new BMessage(kMsgEditReset));
	fStatus = new BStringView("status", "");
	fStatus->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DISABLED_LABEL_TINT);

	float spacing = S(6);
	BLayoutBuilder::Group<>(this, B_VERTICAL, spacing)
		.SetInsets(S(14), S(12), S(14), S(12))
		.Add(Heading("Crop and straighten"))
		.AddGroup(B_HORIZONTAL, spacing)
			.Add(fCrop)
			.Add(fAspectField)
		.End()
		.AddGroup(B_HORIZONTAL, spacing)
			.Add(left)
			.Add(right)
			.Add(flip)
		.End()
		.Add(fStraighten)
		.AddStrut(S(4))
		.Add(new BSeparatorView(B_HORIZONTAL))
		.Add(Heading("Light"))
		.Add(fExposure)
		.Add(fContrast)
		.Add(fHighlights)
		.Add(fShadows)
		.AddStrut(S(4))
		.Add(new BSeparatorView(B_HORIZONTAL))
		.Add(Heading("Colour"))
		.Add(fSaturation)
		.Add(fWarmth)
		.Add(fTint)
		.AddStrut(S(4))
		.AddGroup(B_HORIZONTAL, spacing)
			.Add(fEnhance)
			.Add(fReset)
		.End()
		.Add(fStatus)
		.AddGlue();
}


BSlider*
EditPanel::_Slider(const char* label, int32 minimum, int32 maximum)
{
	BSlider* slider = new BSlider(label, label, new BMessage(kMsgSlider), minimum,
		maximum, B_HORIZONTAL);
	slider->SetModificationMessage(new BMessage(kMsgSlider));
	slider->SetHashMarks(B_HASH_MARKS_NONE);
	slider->SetValue(0);
	return slider;
}


void
EditPanel::AttachedToWindow()
{
	BView::AttachedToWindow();
	BSlider* sliders[] = {fStraighten, fExposure, fContrast, fHighlights, fShadows,
		fSaturation, fWarmth, fTint};
	for (BSlider* slider : sliders)
		slider->SetTarget(this);
	fCrop->SetTarget(this);
	fAspectField->Menu()->SetTargetForItems(this);
	BView* turnLeft = FindView("left");
	BView* turnRight = FindView("right");
	BView* flip = FindView("flip");
	if (turnLeft != nullptr)
		dynamic_cast<BButton*>(turnLeft)->SetTarget(this);
	if (turnRight != nullptr)
		dynamic_cast<BButton*>(turnRight)->SetTarget(this);
	if (flip != nullptr)
		dynamic_cast<BButton*>(flip)->SetTarget(this);
	fEnhance->SetTarget(Window());
	fReset->SetTarget(Window());
}


void
EditPanel::SetParams(const photos::EditParams& params)
{
	fRotation = params.rotation;
	fFlip = params.flip;
	fStraighten->SetValue((int32)lroundf(params.straighten * 10));
	fExposure->SetValue((int32)lroundf(params.exposure * 100));
	fContrast->SetValue((int32)lroundf(params.contrast * 100));
	fHighlights->SetValue((int32)lroundf(params.highlights * 100));
	fShadows->SetValue((int32)lroundf(params.shadows * 100));
	fSaturation->SetValue((int32)lroundf(params.saturation * 100));
	fWarmth->SetValue((int32)lroundf(params.warmth * 100));
	fTint->SetValue((int32)lroundf(params.tint * 100));
}


photos::EditParams
EditPanel::Params(const photos::EditParams& base) const
{
	photos::EditParams params = base;
	params.rotation = fRotation;
	params.flip = fFlip;
	params.straighten = fStraighten->Value() / 10.0f;
	params.exposure = fExposure->Value() / 100.0f;
	params.contrast = fContrast->Value() / 100.0f;
	params.highlights = fHighlights->Value() / 100.0f;
	params.shadows = fShadows->Value() / 100.0f;
	params.saturation = fSaturation->Value() / 100.0f;
	params.warmth = fWarmth->Value() / 100.0f;
	params.tint = fTint->Value() / 100.0f;
	return params;
}


void
EditPanel::SetBusy(bool busy, const char* text)
{
	fStatus->SetText(text != nullptr ? text : "");
	fEnhance->SetEnabled(!busy);
	fReset->SetEnabled(!busy);
}


bool
EditPanel::CropMode() const
{
	return fCrop->Value() == B_CONTROL_ON;
}


void
EditPanel::SetCropMode(bool on)
{
	fCrop->SetValue(on ? B_CONTROL_ON : B_CONTROL_OFF);
	fAspectField->SetEnabled(on);
}


void
EditPanel::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgSlider:
			Window()->PostMessage(kMsgEditChanged);
			break;

		case kMsgCropToggle:
		{
			fAspectField->SetEnabled(CropMode());
			BMessage crop(kMsgEditCrop);
			crop.AddBool("on", CropMode());
			Window()->PostMessage(&crop);
			break;
		}

		case kMsgAspect:
		{
			fAspect = message->GetFloat("aspect", 0);
			BMessage crop(kMsgEditCrop);
			crop.AddBool("on", CropMode());
			crop.AddBool("aspect", true);
			Window()->PostMessage(&crop);
			break;
		}

		case kMsgTurnLeft:
		case kMsgTurnRight:
		{
			// The window turns the crop frame with the picture.
			BMessage turn(kMsgEditChanged);
			turn.AddInt32("turn", message->what == kMsgTurnLeft ? -1 : 1);
			Window()->PostMessage(&turn);
			break;
		}

		case kMsgFlip:
		{
			BMessage flip(kMsgEditChanged);
			flip.AddBool("flip", true);
			Window()->PostMessage(&flip);
			break;
		}

		default:
			BView::MessageReceived(message);
	}
}

}	// namespace ui
