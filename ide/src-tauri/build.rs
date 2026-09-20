use tauri_build::TauriBuilder;

fn main() {
    TauriBuilder::new()
        .invoke_handler(tauri::generate_handler![])
        .build()
        .context("failed to run tauri-build")
        .unwrap();
}
