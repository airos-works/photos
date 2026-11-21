// What the views share: the scale of the interface, message codes, small
// drawing helpers.
#pragma once

#include <Font.h>
#include <Rect.h>
#include <String.h>
#include <View.h>

#include <cstdint>
#include <string>

namespace ui {

// Sizes are written for a 12 point plain font and scaled with the system's.
float Scale();
inline float S(float value) { return floorf(value * Scale() + 0.5f); }

enum {
	// Sidebar: "item" (int32, SidebarItem), "argument" (int64).
	kMsgShowView = 'shVw',
	// Header and menus.
	kMsgGroupMode = 'grpM',			// "mode" (int32, GroupMode)
	kMsgTileSize = 'tlSz',			// "size" (int32)
	kMsgZoomIn = 'zmIn',
	kMsgZoomOut = 'zmOt',
	kMsgOpenPhoto = 'opPh',			// "id" (int64)
	kMsgCloseViewer = 'clVw',
	kMsgPrevious = 'prev',
	kMsgNext = 'next',
	kMsgSelectionChanged = 'selC',
	kMsgSelectAll = 'selA',
	kMsgToggleFavorite = 'favT',
	kMsgMoveToTrash = 'trsh',
	kMsgRestore = 'rstr',
	kMsgDeleteForever = 'delF',
	kMsgEmptyTrash = 'empT',
	kMsgRotateLeft = 'rotL',
	kMsgRotateRight = 'rotR',
	kMsgEdit = 'edit',
	kMsgEditDone = 'edDn',
	kMsgEditCancel = 'edCn',
	kMsgEditChanged = 'edCh',		// the panel's values changed
	kMsgEditCrop = 'edCr',			// "on" (bool)
	kMsgEditEnhance = 'edEn',
	kMsgEditReset = 'edRs',
	kMsgEditRendered = 'edRn',		// "bitmap" (pointer), "serial" (int32)
	kMsgEditSaved = 'edSv',			// "ok" (bool), "error"
	kMsgEditSourceReady = 'edSr',	// "ok" (bool), "error"
	kMsgCropChanged = 'crCh',		// the crop rectangle was dragged
	kMsgEditInPhotoCraft = 'edPc',
	kMsgOpenInLightCraft = 'opLc',
	kMsgEditorOpened = 'edOp',		// "error" when it could not be
	kMsgRevert = 'rvrt',
	kMsgShowVersion = 'shVr',		// "version" (int64)
	kMsgDeleteVersion = 'dlVr',		// "version" (int64)
	kMsgToggleInfo = 'tgIn',
	kMsgToggleFaces = 'tgFc',
	kMsgDownloadOriginal = 'dlOr',
	kMsgOriginalFetched = 'orFt',	// "id", "error"
	kMsgImportFiles = 'imFl',
	kMsgExport = 'expt',
	kMsgExportTo = 'expT',			// from the file panel: "refs"
	kMsgExportDone = 'expD',		// "count", "failed"
	kMsgSyncNow = 'sync',
	kMsgSettings = 'sett',
	kMsgSettingsChanged = 'setC',
	kMsgShowInTracker = 'shTr',
	// People.
	kMsgOpenPerson = 'opPe',		// "person" (int64)
	kMsgNameCluster = 'nmCl',		// "cluster" (int64)
	kMsgNameFace = 'nmFc',			// "face" (int64)
	kMsgNameChosen = 'nmCh',		// "name", plus what was asked
	kMsgConfirmFace = 'cfFc',		// "face", "person"
	kMsgRejectFace = 'rjFc',		// "face", "person"
	kMsgRenamePerson = 'rnPe',		// "person"
	kMsgDeletePerson = 'dlPe',		// "person"
	kMsgScanFaces = 'scFc',
	// Cameras.
	kMsgOpenCamera = 'opCm',		// "key"
	kMsgCameraListed = 'cmLs',
	kMsgCameraThumb = 'cmTh',
	kMsgCameraImportNew = 'cmIn',
	kMsgCameraImportSelected = 'cmIs',
	kMsgCameraImportAll = 'cmIa',
	// Scripting (App.cpp lists the commands for hey): the window's state,
	// selecting a photo ("id"), setting the editor's values ("json").
	kMsgTestQuery = 'tstQ',
	kMsgTestSelect = 'tstS',
	kMsgTestEdit = 'tstE'
};

enum SidebarItem {
	kItemPhotos = 0,
	kItemRecents,
	kItemFavorites,
	kItemPeople,
	kItemEdited,
	kItemLastImport,
	kItemTrash,
	kItemPerson,		// argument: person id
	kItemCamera,		// argument: index of the camera
	kItemImport			// argument: import id
};

enum GroupMode {
	kGroupYears = 0,
	kGroupMonths,
	kGroupDays,
	kGroupNone
};

// Text helpers.
BString MonthName(int month);						// "March"
BString DayTitle(int64_t taken);					// "Saturday, 9 March 2024"
BString MonthTitle(int64_t taken);					// "March 2024"
BString DateTimeText(int64_t taken);				// "9 March 2024, 14:05"
BString CountText(int count, const char* one, const char* many);	// "1,234 photos"
BString Number(int value);							// "1,234"

// Draws text cut to `width` with an ellipsis.
void DrawTruncated(BView* view, const char* text, BPoint where, float width);
// A rectangle with round corners, filled.
void FillRound(BView* view, BRect rect, float radius);
void StrokeRound(BView* view, BRect rect, float radius);
// A heart centred in rect.
void DrawHeart(BView* view, BRect rect, bool filled);

}	// namespace ui
