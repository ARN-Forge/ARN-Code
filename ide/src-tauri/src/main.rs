// Learn more about Tauri commands at https://tauri.app/develop/calling-rust/
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};
use tauri::State;
use std::sync::Mutex;
use serde_json::json;

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
}

#[tauri::command]
pub fn set_project_root(state: State<AppState>, path: String) -> Result<String, String> {
    let path_buf = PathBuf::from(&path);

    // Verify the path exists and is a directory
    if !path_buf.is_dir() {
        return Err("Path does not exist or is not a directory".to_string());
    }

    let mut root = state.project_root.lock().unwrap();
    *root = Some(path_buf);

    Ok(path)
}

#[tauri::command]
pub fn get_project_root(state: State<AppState>) -> Result<Option<String>, String> {
    let root = state.project_root.lock().unwrap();
    Ok(root.as_ref().map(|p| p.to_string_lossy().to_string()))
}

fn build_file_tree(path: &Path, project_root: &Path, max_depth: usize, current_depth: usize) -> Result<FileTreeNode, String> {
    if current_depth > max_depth {
        return Err("Max depth exceeded".to_string());
    }

    let name = path.file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_else(|| ".".to_string());

    let is_dir = path.is_dir();

    let mut children = None;
    if is_dir && current_depth < max_depth {
        let mut child_list = Vec::new();

        // Skip hidden dirs and git, node_modules, etc
        let skip_dirs = [".git", "node_modules", ".vs", "build", "dist", ".idea", "target"];

        match fs::read_dir(path) {
            Ok(entries) => {
                for entry in entries.flatten() {
                    let entry_path = entry.path();
                    if let Some(name) = entry_path.file_name() {
                        let name_str = name.to_string_lossy();
                        if name_str.starts_with('.') || skip_dirs.contains(&name_str.as_ref()) {
                            continue;
                        }

                        if let Ok(node) = build_file_tree(&entry_path, project_root, max_depth, current_depth + 1) {
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

        if !child_list.is_empty() {
            children = Some(child_list);
        }
    }

    Ok(FileTreeNode {
        name,
        path: path.to_string_lossy().to_string(),
        is_dir,
        children,
    })
}

#[tauri::command]
pub fn list_files(state: State<AppState>) -> Result<Option<FileTreeNode>, String> {
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
pub fn read_file(state: State<AppState>, path: String) -> Result<FileContent, String> {
    let root = state.project_root.lock().unwrap();

    let root_path = root.as_ref()
        .ok_or("No project root set")?;

    let file_path = PathBuf::from(&path);

    // Security: ensure file is within project root
    let canonical_root = root_path.canonicalize()
        .map_err(|e| format!("Failed to canonicalize root: {}", e))?;

    let canonical_file = file_path.canonicalize()
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

    let content = fs::read_to_string(&canonical_file)
        .map_err(|e| format!("Failed to read file: {}", e))?;

    Ok(FileContent {
        path,
        content,
    })
}

#[tauri::command]
pub fn write_file(state: State<AppState>, path: String, content: String) -> Result<String, String> {
    let root = state.project_root.lock().unwrap();

    let root_path = root.as_ref()
        .ok_or("No project root set")?;

    let file_path = PathBuf::from(&path);

    // Security: ensure file is within project root
    let canonical_root = root_path.canonicalize()
        .map_err(|e| format!("Failed to canonicalize root: {}", e))?;

    let canonical_file = file_path.canonicalize()
        .map_err(|e| {
            // File may not exist yet, so we check differently
            if file_path.parent().unwrap_or(Path::new("/")).starts_with(root_path) {
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

    fs::write(&canonical_file, content)
        .map_err(|e| format!("Failed to write file: {}", e))?;

    Ok(canonical_file.to_string_lossy().to_string())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .manage(AppState {
            project_root: Mutex::new(None),
        })
        .invoke_handler(tauri::generate_handler![
            set_project_root,
            get_project_root,
            list_files,
            read_file,
            write_file
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
