#pragma once
#include "../core/window.h"
#include "console/store.h"
#include "console/processor.h"

void win_console_init(Window *w, Rect frame, int z, ConsoleStore* store, ConsoleProcessor* proc);
