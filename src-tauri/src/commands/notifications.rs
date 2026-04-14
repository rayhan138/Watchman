use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct InAppNotification {
    pub id: String,
    #[serde(rename = "type")]
    pub notif_type: String,
    pub category: String,
    pub title: String,
    pub message: String,
    pub actions: Vec<NotifAction>,
    pub timestamp: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NotifAction {
    pub label: String,
    pub action: String,
}

// Notifications are emitted as events from the metrics loop in main.rs
// These commands are just placeholders for the frontend compatibility

#[tauri::command]
pub fn dismiss_notification(_notification_id: String) {
    // No-op, handled in frontend
}

#[tauri::command]
pub fn notification_action(_notification_id: String, _action: String) {
    // No-op, handled in frontend
}
