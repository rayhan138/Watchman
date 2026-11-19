#pragma once
#include <windows.h>

struct TaskbarPlacement {
    int x;
    int y;
    int width;
    int height;
    int edge;
};

struct TaskbarMetrics {
    int edge = 0; // 0 bottom, 1 top, 2 left, 3 right
    int thickness = 0;
    unsigned int dpi = 96;
};

extern "C" {
    int tm_get_taskbar_metrics(TaskbarMetrics* metrics);
    int tm_set_widget_layered_style(int enabled);
    int tm_set_widget_manual_layered_paint(int enabled);
    int tm_apply_widget_styles(void* widget_hwnd);
    int tm_embed_widget(void* widget_hwnd, int preferred_width, int preferred_height, TaskbarPlacement* placement);
    // Returns 1 only when both layout and widget cleanup are complete (or unnecessary).
    // A 0 requires retry, including while the widget is disabled/hidden.
    int tm_restore_taskbar_layout();
    int tm_set_widget_context_menu_open(void* owner_hwnd, int open);
    int tm_set_widget_command_target(void* target_hwnd, unsigned int context_menu_message, unsigned int history_message);
    int tm_take_widget_context_menu_request();
    int tm_take_widget_double_click_request();
    int tm_should_widget_be_visible(void* widget_hwnd, void* main_hwnd, int* visible);
}
