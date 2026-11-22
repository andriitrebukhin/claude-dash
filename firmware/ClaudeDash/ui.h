#pragma once
#include "dash_state.h"
void ui_init();
void ui_update(const DashState &s);
void ui_tick();   // clock etc., call a few times per second
