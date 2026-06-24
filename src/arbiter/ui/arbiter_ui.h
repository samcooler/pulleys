#pragma once
#include <lvgl.h>

void ui_init();
void ui_refresh();        // call every ~500ms — updates list + detail labels
void ui_animate();        // call every loop iteration — drives pattern animation
bool ui_needs_refresh();  // true after a tap, so loop can refresh immediately
