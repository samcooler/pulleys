/**
 * LVGL 8.x configuration for the Arbiter (ESP32-S3 4.3" 800×480 display).
 * Stripped to the minimum needed to get a hello-world rendering.
 * Increase LV_MEM_SIZE and enable features as needed.
 */

#if 1  /* Set to "1" to enable content */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* ── Color depth ─────────────────────────────────────────────────────────── */
#define LV_COLOR_DEPTH 16

/* ── Memory — route LVGL heap to PSRAM to stay out of tight internal DRAM ── */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE          "esp_heap_caps.h"
#define LV_MEM_CUSTOM_ALLOC(size)      heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_MEM_CUSTOM_FREE(p)          free(p)
#define LV_MEM_CUSTOM_REALLOC(p, size) heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* ── HAL ─────────────────────────────────────────────────────────────────── */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE <Arduino.h>
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

/* ── Display resolution (informational; actual res set via lv_disp_drv_t) ─ */
#define LV_HOR_RES_MAX 800
#define LV_VER_RES_MAX 480

/* ── Font ────────────────────────────────────────────────────────────────── */
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_DEFAULT       &lv_font_montserrat_16

/* ── Logging ─────────────────────────────────────────────────────────────── */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* ── Widgets (enable only what the test uses) ────────────────────────────── */
#define LV_USE_LABEL  1
#define LV_USE_BTN    1
#define LV_USE_CONT   1

/* ── Required stubs ──────────────────────────────────────────────────────── */
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0
#define LV_USE_REFR_DEBUG   0
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

#define LV_SPRINTF_CUSTOM 0
#define LV_USE_USER_DATA  1

#define LV_DISP_DEF_REFR_PERIOD  20
#define LV_INDEV_DEF_READ_PERIOD 30

#define LV_DPI_DEF 130

#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE 0

#define LV_USE_ANIMATION 1
#define LV_USE_OBJ_REALIGN 0

#define LV_USE_EXTRA_ATTRS 0
#define LV_USE_GPU_STM32_DMA2D 0
#define LV_USE_GPU_NXP_PXP     0
#define LV_USE_GPU_NXP_VG_LITE  0
#define LV_USE_GPU_SDL          0

#define LV_USE_FS_STDIO   0
#define LV_USE_FS_POSIX   0
#define LV_USE_FS_WIN32   0
#define LV_USE_FS_FATFS   0

#define LV_USE_PNG  0
#define LV_USE_BMP  0
#define LV_USE_JPG  0
#define LV_USE_GIF  0

#define LV_USE_FLEX  1
#define LV_USE_GRID  0

#define LV_USE_CALENDAR 0
#define LV_USE_CALENDAR_HEADER_ARROW    0
#define LV_USE_CALENDAR_HEADER_DROPDOWN 0
#define LV_USE_MSGBOX   0
#define LV_USE_SPINBOX  0
#define LV_USE_SPINNER  0
#define LV_USE_TABVIEW  0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN      0
#define LV_USE_SPAN     0
#define LV_USE_LED      0
#define LV_USE_DROPDOWN 0
#define LV_USE_KEYBOARD 0
#define LV_USE_IMG      1
#define LV_USE_LINE     0
#define LV_USE_ARC      0
#define LV_USE_METER    0
#define LV_USE_TEXTAREA 0
#define LV_USE_TABLE    0
#define LV_USE_CHECKBOX 0
#define LV_USE_CHART    1
#define LV_USE_BAR      0
#define LV_USE_ROLLER   0
#define LV_USE_LIST     0
#define LV_USE_SLIDER   0
#define LV_USE_SWITCH   0

#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS        0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_DEMO_BENCHMARK      0
#define LV_USE_DEMO_STRESS         0
#define LV_USE_DEMO_MUSIC          0

#endif /* LV_CONF_H */
#endif /* Enable content */
