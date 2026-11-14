//! JSONL transport only. Provider, conversation and file tools live in C++ ARN.
use serde_json::{json, Value};
use std::{
    path::{Path, PathBuf},
    process::Stdio,
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Arc,
    },
};
use tauri::{Emitter, Manager};
use tokio::{
    io::{AsyncBufReadExt, AsyncWriteExt, BufReader},
    process::{Child, ChildStdin, Command},
    sync::{oneshot, Mutex},
    time::{timeout, Duration},
};
static SEQUENCE: AtomicU64 = AtomicU64::new(1);
type Reply = oneshot::Sender<Result<Value, String>>;
struct Pending {
    id: String,
    reply: Reply,
}
pub struct ArnBridge {
    child: Mutex<Child>,
    stdin: Mutex<Option<ChildStdin>>,
    pending: Mutex<Option<Pending>>,
    confirmation: Mutex<Option<String>>,
    pub session: String,
    pub alive: AtomicBool,
    pub connected: AtomicBool,
}
impl ArnBridge {
    pub async fn spawn(root: &Path, app: tauri::AppHandle) -> Result<Arc<Self>, String> {
        let binary = resolve_binary(&app)?;
        Self::spawn_process(root, binary, move |event| {
            let _ = app.emit("agent-event", event);
        })
        .await
    }
    async fn spawn_process(
        root: &Path,
        binary: PathBuf,
        emit: impl Fn(Value) + Send + 'static,
    ) -> Result<Arc<Self>, String> {
        let mut command = Command::new(binary);
        command
            .arg("--server")
            .current_dir(root)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .kill_on_drop(true);
        #[cfg(windows)]
        command.creation_flags(0x08000000); // CREATE_NO_WINDOW
        let mut child = command
            .spawn()
            .map_err(|e| format!("Cannot start ARN: {e}. Build ARN or set ARN_BIN."))?;
        let input = child.stdin.take().ok_or("ARN stdin unavailable")?;
        let output = child.stdout.take().ok_or("ARN stdout unavailable")?;
        let bridge = Arc::new(Self {
            child: Mutex::new(child),
            stdin: Mutex::new(Some(input)),
            pending: Mutex::new(None),
            confirmation: Mutex::new(None),
            session: format!("server-{}", SEQUENCE.fetch_add(1, Ordering::Relaxed)),
            alive: AtomicBool::new(true),
            connected: AtomicBool::new(false),
        });
        let weak = Arc::downgrade(&bridge);
        let (ready_tx, ready_rx) = oneshot::channel();
        tokio::spawn(async move {
            let mut ready = Some(ready_tx);
            let mut lines = BufReader::new(output).lines();
            while let Ok(Some(line)) = lines.next_line().await {
                let Some(bridge) = weak.upgrade() else { break };
                let Ok(mut event) = serde_json::from_str::<Value>(&line) else {
                    bridge.fail("Invalid JSON from ARN").await;
                    break;
                };
                let kind = event["type"].as_str().unwrap_or("").to_owned();
                if kind == "ready" {
                    if event["protocol"] != 2 {
                        bridge
                            .fail("ARN protocol mismatch. Rebuild the C++ backend.")
                            .await;
                        break;
                    }
                    if let Some(tx) = ready.take() {
                        let _ = tx.send(());
                    }
                    continue;
                }
                let mut pending = bridge.pending.lock().await;
                let matches = pending
                    .as_ref()
                    .map(|p| Some(p.id.as_str()) == event["requestId"].as_str())
                    .unwrap_or(false);
                if !matches {
                    continue;
                }
                if kind == "confirmation_required" {
                    *bridge.confirmation.lock().await = event["id"].as_str().map(str::to_owned);
                } else if kind == "confirmation_resolved" {
                    bridge.confirmation.lock().await.take();
                } else if [
                    "error",
                    "cancelled",
                    "complete",
                    "configured",
                    "models_listed",
                    "session_cleared",
                ]
                .contains(&kind.as_str())
                {
                    bridge.confirmation.lock().await.take();
                    if kind == "configured" {
                        bridge.connected.store(true, Ordering::SeqCst);
                    }
                    if let Some(p) = pending.take() {
                        let reply = if kind == "error" {
                            Err(event["message"].as_str().unwrap_or("ARN error").to_owned())
                        } else if kind == "cancelled" {
                            Err("Request cancelled".into())
                        } else {
                            Ok(event.clone())
                        };
                        let _ = p.reply.send(reply);
                    }
                }
                event["session"] = json!(bridge.session);
                emit(event);
            }
            if let Some(bridge) = weak.upgrade() {
                bridge
                    .fail("ARN disconnected. Reopen the project to restart the backend.")
                    .await;
                emit(json!({"type":"disconnected", "session":bridge.session}));
            }
        });
        if timeout(Duration::from_secs(5), ready_rx)
            .await
            .ok()
            .and_then(Result::ok)
            .is_none()
        {
            let exit_code = bridge
                .child
                .lock()
                .await
                .try_wait()
                .ok()
                .flatten()
                .and_then(|status| status.code());
            bridge.shutdown().await;
            return Err(startup_error(exit_code));
        }
        Ok(bridge)
    }
    async fn fail(&self, message: &str) {
        self.alive.store(false, Ordering::SeqCst);
        self.connected.store(false, Ordering::SeqCst);
        self.confirmation.lock().await.take();
        if let Some(p) = self.pending.lock().await.take() {
            let _ = p.reply.send(Err(message.into()));
        }
    }
    async fn write(&self, value: Value) -> Result<(), String> {
        if !self.alive.load(Ordering::SeqCst) {
            return Err("ARN is not running. Reopen the project.".into());
        }
        let mut guard = self.stdin.lock().await;
        let input = guard.as_mut().ok_or("ARN stdin closed")?;
        let data = format!("{value}\n");
        input
            .write_all(data.as_bytes())
            .await
            .map_err(|_| "Cannot write to ARN".to_string())
    }
    pub async fn request(&self, mut command: Value) -> Result<Value, String> {
        let id = format!("req-{}", SEQUENCE.fetch_add(1, Ordering::Relaxed));
        let (tx, rx) = oneshot::channel();
        {
            let mut pending = self.pending.lock().await;
            if pending.is_some() {
                return Err("ARN is busy".into());
            }
            *pending = Some(Pending {
                id: id.clone(),
                reply: tx,
            });
        }
        if command["type"] == "configure" {
            self.connected.store(false, Ordering::SeqCst);
        }
        let seconds = if command["type"] == "prompt" {
            900
        } else {
            120
        };
        command["id"] = json!(id);
        if let Err(error) = self.write(command).await {
            self.fail(&error).await;
            return Err(error);
        }
        match timeout(Duration::from_secs(seconds), rx).await {
            Ok(Ok(result)) => result,
            _ => {
                self.shutdown().await;
                Err("ARN response timed out or transport closed. Reopen the project.".into())
            }
        }
    }
    pub async fn cancel(&self) -> Result<(), String> {
        let id = self.pending.lock().await.as_ref().map(|p| p.id.clone());
        if let Some(id) = id {
            self.write(json!({"type":"cancel", "id":id})).await?;
        }
        Ok(())
    }
    pub async fn confirm(&self, id: String, approved: bool) -> Result<(), String> {
        let mut pending = self.confirmation.lock().await;
        if pending.as_ref() != Some(&id) {
            return Err("This confirmation is no longer pending".into());
        }
        pending.take();
        self.write(json!({"type":"confirmation_response", "id":id, "approved":approved}))
            .await
    }
    pub async fn shutdown(&self) {
        let _ = self.cancel().await;
        self.stdin.lock().await.take(); // EOF lets ARN cancel confirmation and network work.
        let mut child = self.child.lock().await;
        if timeout(Duration::from_secs(2), child.wait()).await.is_err() {
            let _ = child.kill().await;
            let _ = child.wait().await;
        }
        self.fail("ARN stopped (project changed or IDE closed)")
            .await;
    }
}
/// Windows status codes of a process that could not load its DLLs: a DLL is
/// missing, or too old to have a function the program needs.
const STATUS_DLL_NOT_FOUND: u32 = 0xC000_0135;
const STATUS_ENTRYPOINT_NOT_FOUND: u32 = 0xC000_0139;
/// Microsoft's link to the latest Visual C++ Redistributable for x64. ARN
/// does not ship the Visual C++ runtime; users install it from Microsoft.
const VC_REDIST_URL: &str = "https://aka.ms/vc14/vc_redist.x64.exe";

/// The error for an ARN that did not report ready; `exit_code` is its exit
/// code if it has already ended. A DLL that could not be loaded (on Windows,
/// usually the Microsoft Visual C++ runtime) gets a message that says what
/// to install.
fn startup_error(exit_code: Option<i32>) -> String {
    match exit_code.map(|code| code as u32) {
        Some(status @ (STATUS_DLL_NOT_FOUND | STATUS_ENTRYPOINT_NOT_FOUND)) => format!(
            "ARN could not start: a DLL it needs is missing or too old (Windows status 0x{status:08X}). \
             ARN needs the Microsoft Visual C++ Redistributable (x64): install it from {VC_REDIST_URL}, \
             then reopen the project."
        ),
        _ => "ARN did not provide protocol 2 ready event. Rebuild ARN and check runtime DLLs.".into(),
    }
}

fn resolve_binary(app: &tauri::AppHandle) -> Result<PathBuf, String> {
    if let Some(path) = std::env::var_os("ARN_BIN") {
        return PathBuf::from(path)
            .canonicalize()
            .map_err(|_| "ARN_BIN does not point to an existing ARN executable".into());
    }
    let name = if cfg!(windows) { "arn.exe" } else { "arn" };
    let mut candidates = Vec::new();
    if let Ok(dir) = app.path().resource_dir() {
        candidates.push(dir.join(name));
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            candidates.push(dir.join(name));
        }
    }
    if cfg!(debug_assertions) {
        let repo = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        for dir in ["build-ide/Release", "build-ide", "build/Release", "build"] {
            candidates.push(repo.join(dir).join(name));
        }
    }
    if let Some(path) = std::env::var_os("PATH") {
        candidates.extend(std::env::split_paths(&path).map(|p| p.join(name)));
    }
    candidates
        .into_iter()
        .find(|p| p.is_file())
        .and_then(|p| p.canonicalize().ok())
        .ok_or(
            "ARN executable not found. Build C++ ARN or set ARN_BIN to its absolute path.".into(),
        )
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn startup_error_names_the_visual_cpp_redistributable() {
        let missing = startup_error(Some(STATUS_DLL_NOT_FOUND as i32));
        assert!(missing.contains("0xC0000135"), "{missing}");
        assert!(missing.contains(VC_REDIST_URL), "{missing}");
        let too_old = startup_error(Some(STATUS_ENTRYPOINT_NOT_FOUND as i32));
        assert!(
            too_old.contains("0xC0000139") && too_old.contains(VC_REDIST_URL),
            "{too_old}"
        );
        for other in [Some(1), Some(-1), None] {
            let message = startup_error(other);
            assert!(message.contains("protocol 2 ready event"), "{message}");
            assert!(!message.contains(VC_REDIST_URL), "{message}");
        }
    }
    #[tokio::test]
    async fn transport_lifecycle_and_confirmation() {
        let binary = PathBuf::from(
            std::env::var_os("ARN_TEST_BIN")
                .expect("Set ARN_TEST_BIN to the C++ arn_server_fixture executable"),
        );
        let root = std::env::temp_dir().join(format!(
            "arn-bridge-test-{}-{}",
            std::process::id(),
            SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&root).unwrap();
        let (tx, mut events) = tokio::sync::mpsc::unbounded_channel();
        let bridge = ArnBridge::spawn_process(&root, binary.clone(), move |event| {
            let _ = tx.send(event);
        })
        .await
        .unwrap();
        assert!(!bridge.connected.load(Ordering::SeqCst));
        let models = bridge
            .request(json!({"type":"configure", "provider":"gemini", "apiKey":"fixture-only"}))
            .await
            .unwrap();
        assert_eq!(models["models"], json!(["fixture-model"]));
        assert!(!bridge.connected.load(Ordering::SeqCst));
        bridge
            .request(json!({"type":"select_model", "model":"fixture-model"}))
            .await
            .unwrap();
        assert!(bridge.connected.load(Ordering::SeqCst));
        let worker = bridge.clone();
        let request = tokio::spawn(async move {
            worker.request(json!({"type":"prompt", "text":json!({"name":"write_file", "arguments":{"path":"file.txt", "content":"approved"}}).to_string()})).await
        });
        let mut received_progress = false;
        let proposal = timeout(Duration::from_secs(5), async {
            loop {
                let event = events.recv().await.unwrap();
                if event["type"] == "progress" {
                    assert_eq!(event["session"], bridge.session);
                    received_progress = true;
                }
                if event["type"] == "confirmation_required" {
                    break event;
                }
            }
        })
        .await
        .unwrap();
        assert!(
            received_progress,
            "Progress must be forwarded without completing the request"
        );
        assert!(!root.join("file.txt").exists());
        assert!(bridge
            .request(json!({"type":"clear_session"}))
            .await
            .is_err());
        assert!(bridge.confirm("stale".into(), true).await.is_err());
        bridge
            .confirm(proposal["id"].as_str().unwrap().into(), true)
            .await
            .unwrap();
        request.await.unwrap().unwrap();
        assert_eq!(
            std::fs::read_to_string(root.join("file.txt")).unwrap(),
            "approved"
        );
        let worker = bridge.clone();
        let request = tokio::spawn(async move {
            worker
                .request(json!({"type":"prompt", "text":"wait"}))
                .await
        });
        timeout(Duration::from_secs(5), async {
            loop {
                if events.recv().await.unwrap()["text"] == "fixture-start" {
                    break;
                }
            }
        })
        .await
        .unwrap();
        bridge.cancel().await.unwrap();
        assert!(request.await.unwrap().unwrap_err().contains("cancelled"));
        bridge.shutdown().await;
        assert!(!bridge.alive.load(Ordering::SeqCst));
        assert!(bridge.child.lock().await.try_wait().unwrap().is_some());
        let (tx, mut events) = tokio::sync::mpsc::unbounded_channel();
        let replacement = ArnBridge::spawn_process(&root, binary, move |event| {
            let _ = tx.send(event);
        })
        .await
        .unwrap();
        assert_ne!(bridge.session, replacement.session);
        replacement
            .request(json!({"type":"configure", "provider":"gemini", "apiKey":"fixture-only"}))
            .await
            .unwrap();
        replacement
            .request(json!({"type":"select_model", "model":"fixture-model"}))
            .await
            .unwrap();
        let worker = replacement.clone();
        let pending = tokio::spawn(async move {
            worker.request(json!({"type":"prompt", "text":json!({"name":"write_file", "arguments":{"path":"never.txt", "content":"unapproved"}}).to_string()})).await
        });
        timeout(Duration::from_secs(5), async {
            loop {
                if events.recv().await.unwrap()["type"] == "confirmation_required" {
                    break;
                }
            }
        })
        .await
        .unwrap();
        replacement.shutdown().await;
        assert!(pending.await.unwrap().is_err());
        assert!(!root.join("never.txt").exists());
        assert!(replacement.child.lock().await.try_wait().unwrap().is_some());
        std::fs::remove_file(root.join("file.txt")).unwrap();
        std::fs::remove_dir(root).unwrap();
    }
}
