// The editor's controls: crop and straighten, light, colour.
#pragma once

#include "Ui.h"
#include "core/Edit.h"

#include <View.h>

class BButton;
class BCheckBox;
class BMenuField;
class BSlider;
class BStringView;

namespace ui {

class EditPanel : public BView {
public:
	EditPanel();

	// Shows the values (without telling anyone).
	void SetParams(const photos::EditParams& params);
	// The values of the controls on top of `base` (which keeps the crop).
	photos::EditParams Params(const photos::EditParams& base) const;
	void SetBusy(bool busy, const char* text);
	bool CropMode() const;
	void SetCropMode(bool on);
	// Width / height the crop is held to; 0: free; -1: the picture's own.
	float CropAspect() const { return fAspect; }

	virtual void AttachedToWindow();
	virtual void MessageReceived(BMessage* message);

private:
	BSlider* _Slider(const char* label, int32 minimum, int32 maximum);

	BCheckBox* fCrop;
	BMenuField* fAspectField;
	BSlider* fStraighten;
	BSlider* fExposure;
	BSlider* fContrast;
	BSlider* fHighlights;
	BSlider* fShadows;
	BSlider* fSaturation;
	BSlider* fWarmth;
	BSlider* fTint;
	BButton* fEnhance;
	BButton* fReset;
	BStringView* fStatus;
	float fAspect;
	int fRotation;
	bool fFlip;
};

}	// namespace ui
