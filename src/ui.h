// Hangar - main window
#pragma once
#include "common.h"
#include "selfupdate.h"

HWND UiCreate(HINSTANCE inst, int show, const selfupdate::AfterUpdate& after);
bool UiHandleAccelerator(MSG* msg);
