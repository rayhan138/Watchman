#[cfg(target_os = "windows")]
use serde::Serialize;

#[cfg(target_os = "windows")]
use std::ffi::c_void;

#[cfg(target_os = "windows")]
#[repr(C)]
struct NativeWidgetPlacement {
    x: i32,
    y: i32,
    width: i32,
    height: i32,
    edge: i32,
}

#[cfg(target_os = "windows")]
unsafe extern "C" {
    fn tm_apply_widget_styles(widget_hwnd: *mut c_void) -> i32;
    fn tm_embed_widget(
        widget_hwnd: *mut c_void,
        preferred_width: i32,
        preferred_height: i32,
        placement: *mut NativeWidgetPlacement,
    ) -> i32;
    fn tm_restore_taskbar_layout() -> i32;
    fn tm_should_widget_be_visible(
        widget_hwnd: *mut c_void,
        main_hwnd: *mut c_void,
        visible: *mut i32,
    ) -> i32;
}

#[cfg(target_os = "windows")]
#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct WidgetPlacement {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
    pub edge: String,
    pub anchor: String,
}

#[cfg(target_os = "windows")]
#[allow(dead_code)]
pub fn calculate_widget_position() -> Option<(i32, i32)> {
    None
}

#[cfg(target_os = "windows")]
pub fn apply_widget_styles(hwnd_raw: isize) -> bool {
    unsafe { tm_apply_widget_styles(hwnd_raw as *mut c_void) != 0 }
}

#[cfg(target_os = "windows")]
pub fn enforce_widget(
    hwnd_raw: isize,
    preferred_width: i32,
    preferred_height: i32,
) -> Option<WidgetPlacement> {
    let mut placement = NativeWidgetPlacement {
        x: 0,
        y: 0,
        width: 0,
        height: 0,
        edge: 0,
    };

    let ok = unsafe {
        tm_embed_widget(
            hwnd_raw as *mut c_void,
            preferred_width,
            preferred_height,
            &mut placement,
        )
    };

    (ok != 0).then(|| WidgetPlacement {
        x: placement.x,
        y: placement.y,
        width: placement.width,
        height: placement.height,
        edge: edge_name(placement.edge).to_string(),
        anchor: "tray".to_string(),
    })
}

#[cfg(target_os = "windows")]
pub fn restore_taskbar_layout() {
    unsafe {
        let _ = tm_restore_taskbar_layout();
    }
}

#[cfg(target_os = "windows")]
pub fn get_window_size(hwnd_raw: isize) -> Option<(i32, i32)> {
    use windows::Win32::{
        Foundation::{HWND, RECT},
        UI::WindowsAndMessaging::GetWindowRect,
    };

    let hwnd = HWND(hwnd_raw);
    let mut rect = RECT::default();
    unsafe { GetWindowRect(hwnd, &mut rect) }
        .ok()
        .map(|_| (rect.right - rect.left, rect.bottom - rect.top))
}

#[cfg(target_os = "windows")]
pub fn should_widget_be_visible(widget_hwnd_raw: isize, main_hwnd_raw: Option<isize>) -> bool {
    let mut visible = 1;
    let ok = unsafe {
        tm_should_widget_be_visible(
            widget_hwnd_raw as *mut c_void,
            main_hwnd_raw.unwrap_or_default() as *mut c_void,
            &mut visible,
        )
    };

    if ok == 0 {
        true
    } else {
        visible != 0
    }
}

#[cfg(target_os = "windows")]
fn edge_name(edge: i32) -> &'static str {
    match edge {
        1 => "top",
        2 => "left",
        3 => "right",
        _ => "bottom",
    }
}
