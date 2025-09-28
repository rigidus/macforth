#pragma once
#include "window.h"
struct WM;

void input_route_mouse(struct WM*, const InputEvent *e);
void input_route_key(struct WM*, const InputEvent *e);
void input_route_text(struct WM*, const InputEvent *e);
