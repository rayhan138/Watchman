// Re-export the library for Tauri mobile targets (not used on desktop, but needed for compilation)
#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    // Desktop entry point is in main.rs
}
