// Learn more about Tauri commands at https://tauri.app/develop/calling-rust/
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use tauri::{Manager, State};

mod arn_bridge;
use arn_bridge::ArnBridge;

#[derive(Serialize, Deserialize)]
pub struct FileTreeNode {
    name: String,
    path: String,
    is_dir: bool,
    children: Option<Vec<FileTreeNode>>,
}

#[derive(Serialize)]
pub struct FileContent {
    path: String,
    content: String,
}

pub struct AppState {
    project_root: Mutex<Option<PathBuf>>,
    bridge: tokio::sync::Mutex<Option<Arc<ArnBridge>>>,
    agent_busy: AtomicBool,
}

#[tauri::command]
async fn set_project_root(
    state: State<'_, AppState>,
    app: tauri::AppHandle,
    path: String,
) -> Result<String, String> {
    let root = PathBuf::from(path)
        .canonicalize()
        .map_err(|e| format!("Cannot open project: {e}"))?;
    if !root.is_dir() {
        return Err("Project must be a directory".into());
    }
    let mut bridge = state.bridge.lock().await;
    if let Some(old) = bridge.take() {
        old.shutdown().await;
    }
    *state.project_root.lock().unwrap() = Some(root.clone());
    // Editor remains usable if ARN is missing; agent_status exposes the startup error.
    match ArnBridge::spawn(&root, app).await {
        Ok(process) => *bridge = Some(process),
        Err(error) => return Err(error),
    }
    Ok(root.to_string_lossy().to_string())
}

#[tauri::command]
fn get_project_root(state: State<AppState>) -> Result<Option<String>, String> {
    let root = state.project_root.lock().unwrap();
    Ok(root.as_ref().map(|p| p.to_string_lossy().to_string()))
}

fn build_file_tree(
    path: &Path,
    project_root: &Path,
    max_depth: usize,
    current_depth: usize,
) -> Result<FileTreeNode, String> {
    if current_depth > max_depth {
        return Err("Max depth exceeded".to_string());
    }

    let name = path
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_else(|| ".".to_string());

    let resolved = path.canonicalize().map_err(|e| e.to_string())?;
    if !resolved.starts_with(project_root) {
        return Err("Path escapes project".into());
    }
    let is_dir = path.is_dir();

    let mut children = None;
    if is_dir && current_depth < max_depth {
        let mut child_list = Vec::new();

        // Skip hidden dirs and git, node_modules, etc
        let skip_dirs = [
            ".git",
            "node_modules",
            ".vs",
            "build",
            "dist",
            ".idea",
            "target",
        ];

        match fs::read_dir(path) {
            Ok(entries) => {
                for entry in entries.flatten() {
                    let entry_path = entry.path();
                    if let Some(name) = entry_path.file_name() {
                        let name_str = name.to_string_lossy();
                        if name_str.starts_with('.') || skip_dirs.contains(&name_str.as_ref()) {
                            continue;
                        }

                        if let Ok(node) =
                            build_file_tree(&entry_path, project_root, max_depth, current_depth + 1)
                        {
                            child_list.push(node);
                        }
                    }
                }
            }
            Err(_) => {}
        }

        child_list.sort_by(|a, b| {
            if a.is_dir != b.is_dir {
                b.is_dir.cmp(&a.is_dir)
            } else {
                a.name.cmp(&b.name)
            }
        });

        children = Some(child_list);
    }

    Ok(FileTreeNode {
        name,
        path: path.to_string_lossy().to_string(),
        is_dir,
        children,
    })
}

#[tauri::command]
fn list_files(state: State<AppState>) -> Result<Option<FileTreeNode>, String> {
    let root = state.project_root.lock().unwrap();

    match root.as_ref() {
        Some(root_path) => {
            let tree = build_file_tree(root_path, root_path, 3, 0)?;
            Ok(Some(tree))
        }
        None => Ok(None),
    }
}

#[tauri::command]
fn read_file(state: State<AppState>, path: String) -> Result<FileContent, String> {
    let root = state.project_root.lock().unwrap();

    let root_path = root.as_ref().ok_or("No project root set")?;

    let file_path = PathBuf::from(&path);

    // Security: ensure file is within project root
    let canonical_root = root_path
        .canonicalize()
        .map_err(|e| format!("Failed to canonicalize root: {}", e))?;

    let canonical_file = file_path
        .canonicalize()
        .map_err(|e| format!("Failed to canonicalize path: {}", e))?;

    if !canonical_file.starts_with(&canonical_root) {
        return Err("Path escapes project root".to_string());
    }

    if canonical_file.is_dir() {
        return Err("Path is a directory".to_string());
    }

    // Limit file size to 5MB
    let metadata = fs::metadata(&canonical_file)
        .map_err(|e| format!("Failed to read file metadata: {}", e))?;

    if metadata.len() > 5 * 1024 * 1024 {
        return Err("File is too large (>5MB)".to_string());
    }

    let content =
        fs::read_to_string(&canonical_file).map_err(|e| format!("Failed to read file: {}", e))?;

    Ok(FileContent { path, content })
}

#[tauri::command]
fn write_file(state: State<AppState>, path: String, content: String) -> Result<String, String> {
    if state.agent_busy.load(Ordering::SeqCst) {
        return Err("Editor is read-only while the agent is running".into());
    }
    let root = state.project_root.lock().unwrap();

    let root_path = root.as_ref().ok_or("No project root set")?;

    let file_path = PathBuf::from(&path);

    // Security: ensure file is within project root
    let canonical_root = root_path
        .canonicalize()
        .map_err(|e| format!("Failed to canonicalize root: {}", e))?;

    let canonical_file = file_path.canonicalize().map_err(|e| {
        // File may not exist yet, so we check differently
        if file_path
            .parent()
            .unwrap_or(Path::new("/"))
            .starts_with(root_path)
        {
            file_path.to_string_lossy().to_string()
        } else {
            format!("Failed to canonicalize path: {}", e)
        }
    })?;

    if !canonical_file.starts_with(&canonical_root) {
        return Err("Path escapes project root".to_string());
    }

    // Create parent directories if needed
    if let Some(parent) = canonical_file.parent() {
        fs::create_dir_all(parent)
            .map_err(|e| format!("Failed to create parent directories: {}", e))?;
    }

    fs::write(&canonical_file, content).map_err(|e| format!("Failed to write file: {}", e))?;

    Ok(canonical_file.to_string_lossy().to_string())
}

async fn bridge(state: &AppState) -> Result<Arc<ArnBridge>, String> {
    state
        .bridge
        .lock()
        .await
        .clone()
        .ok_or("ARN is unavailable. Open a project after building ARN.".into())
}
async fn session_bridge(state: &AppState, session: &str) -> Result<Arc<ArnBridge>, String> {
    let process = bridge(state).await?;
    if process.session != session {
        return Err("Project changed; this agent command has expired".into());
    }
    Ok(process)
}
#[tauri::command]
async fn get_agent_status(state: State<'_, AppState>) -> Result<Value, String> {
    let process = bridge(&state).await?;
    Ok(
        json!({"connected":process.connected.load(Ordering::SeqCst) && process.alive.load(Ordering::SeqCst), "session":process.session}),
    )
}
#[tauri::command]
async fn configure_agent(
    state: State<'_, AppState>,
    provider: String,
    api_key: String,
    session: String,
) -> Result<Value, String> {
    session_bridge(&state, &session)
        .await?
        .request(json!({"type":"configure", "provider":provider, "apiKey":api_key}))
        .await
}
#[tauri::command]
async fn select_agent_model(
    state: State<'_, AppState>,
    model: String,
    session: String,
) -> Result<Value, String> {
    session_bridge(&state, &session)
        .await?
        .request(json!({"type":"select_model", "model":model}))
        .await
}
#[tauri::command]
async fn list_agent_models(state: State<'_, AppState>, session: String) -> Result<Value, String> {
    session_bridge(&state, &session)
        .await?
        .request(json!({"type":"list_models"}))
        .await
}
struct BusyGuard<'a>(&'a AtomicBool);
impl Drop for BusyGuard<'_> {
    fn drop(&mut self) {
        self.0.store(false, Ordering::SeqCst);
    }
}
#[tauri::command]
async fn send_agent_message(
    state: State<'_, AppState>,
    message: String,
    dirty_paths: Vec<String>,
    session: String,
) -> Result<Value, String> {
    if !dirty_paths.is_empty() {
        return Err("Save or close unsaved tabs before running the agent.".into());
    }
    if state.agent_busy.swap(true, Ordering::SeqCst) {
        return Err("Another agent request is active".into());
    }
    let _guard = BusyGuard(&state.agent_busy);
    session_bridge(&state, &session)
        .await?
        .request(json!({"type":"prompt", "text":message}))
        .await
}
#[tauri::command]
async fn cancel_agent(state: State<'_, AppState>, session: String) -> Result<(), String> {
    session_bridge(&state, &session).await?.cancel().await
}
#[tauri::command]
async fn confirm_agent_change(
    state: State<'_, AppState>,
    id: String,
    approved: bool,
    session: String,
) -> Result<(), String> {
    let process = session_bridge(&state, &session).await?;
    if process.session != session {
        return Err("Project changed; confirmation is no longer valid".into());
    }
    process.confirm(id, approved).await
}
#[tauri::command]
async fn reset_agent_session(state: State<'_, AppState>, session: String) -> Result<Value, String> {
    session_bridge(&state, &session)
        .await?
        .request(json!({"type":"clear_session"}))
        .await
}
#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(AppState {
            project_root: Mutex::new(None),
            bridge: tokio::sync::Mutex::new(None),
            agent_busy: AtomicBool::new(false),
        })
        .invoke_handler(tauri::generate_handler![
            set_project_root,
            get_project_root,
            list_files,
            read_file,
            write_file,
            get_agent_status,
            configure_agent,
            select_agent_model,
            list_agent_models,
            send_agent_message,
            cancel_agent,
            confirm_agent_change,
            reset_agent_session
        ])
        .build(tauri::generate_context!())
        .expect("error building ARN IDE")
        .run(|app, event| {
            if let tauri::RunEvent::Exit = event {
                let state = app.state::<AppState>();
                tauri::async_runtime::block_on(async {
                    if let Some(process) = state.bridge.lock().await.take() {
                        process.shutdown().await;
                    }
                });
            }
        });
}
fn main() {
    run();
}
