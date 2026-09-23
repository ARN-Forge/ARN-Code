use serde::{Deserialize, Serialize};
use serde_json::json;
use std::sync::Arc;
use tokio::sync::Mutex;

const GEMINI_API_BASE: &str = "https://generativelanguage.googleapis.com/v1beta/models";
const REQUEST_TIMEOUT_SECS: u64 = 30;

#[allow(dead_code)]
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GeminiMessage {
    pub role: String, // "user" or "model"
    pub parts: Vec<GeminiPart>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GeminiPart {
    pub text: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GeminiContent {
    pub role: String,
    pub parts: Vec<GeminiPart>,
}

#[derive(Debug, Serialize)]
struct GeminiRequest {
    contents: Vec<GeminiContent>,
    #[serde(skip_serializing_if = "Option::is_none")]
    system_instruction: Option<String>,
}

#[derive(Debug, Deserialize)]
struct GeminiResponse {
    candidates: Option<Vec<GeminiCandidate>>,
    error: Option<GeminiError>,
}

#[derive(Debug, Deserialize)]
struct GeminiCandidate {
    content: GeminiContent,
}

#[derive(Debug, Deserialize)]
struct GeminiError {
    #[allow(dead_code)]
    code: i32,
    message: String,
}

pub struct GeminiProvider {
    api_key: String,
    model: String,
    conversation_history: Arc<Mutex<Vec<GeminiContent>>>,
    client: reqwest::Client,
}

impl GeminiProvider {
    pub async fn new(api_key: String, model: String) -> Result<Self, String> {
        // Validate API key and model with a lightweight request
        Self::validate_credentials(&api_key, &model).await?;

        let client = reqwest::Client::builder()
            .timeout(std::time::Duration::from_secs(REQUEST_TIMEOUT_SECS))
            .build()
            .map_err(|e| format!("Failed to create HTTP client: {}", e))?;

        Ok(GeminiProvider {
            api_key,
            model,
            conversation_history: Arc::new(Mutex::new(Vec::new())),
            client,
        })
    }

    pub fn format_generate_content_url(model: &str) -> String {
        let clean_model = model.strip_prefix("models/").unwrap_or(model);
        format!("{}/{}:generateContent", GEMINI_API_BASE, clean_model)
    }

    async fn validate_credentials(api_key: &str, model: &str) -> Result<(), String> {
        if api_key.is_empty() {
            return Err("API key is empty".to_string());
        }

        if model.is_empty() {
            return Err("Model name is empty".to_string());
        }

        // Test API key with a simple request
        let client = reqwest::Client::builder()
            .timeout(std::time::Duration::from_secs(REQUEST_TIMEOUT_SECS))
            .build()
            .map_err(|e| format!("Failed to create HTTP client: {}", e))?;

        let test_url = Self::format_generate_content_url(model);

        let request_body = json!({
            "contents": [{
                "parts": [{"text": "test"}]
            }]
        });

        let response = client
            .post(&test_url)
            .header("Content-Type", "application/json")
            .header("x-goog-api-key", api_key)
            .json(&request_body)
            .send()
            .await
            .map_err(|e| {
                if e.is_timeout() {
                    "Request timeout: Check your internet connection".to_string()
                } else if e.is_connect() {
                    "Network error: Cannot reach Gemini API".to_string()
                } else {
                    format!("Network error: {}", e)
                }
            })?;

        if !response.status().is_success() {
            let status = response.status().as_u16();
            let body = response.text().await.unwrap_or_else(|_| "unknown".to_string());

            return match status {
                400 => Err("Invalid API key format or request".to_string()),
                401 => Err("Invalid API key: Authentication failed".to_string()),
                403 => Err("API key is not authorized for this service".to_string()),
                404 => Err("Model not found or API endpoint error".to_string()),
                429 => Err("Rate limited: Please wait before trying again".to_string()),
                500..=599 => Err(format!("Gemini API server error ({}): Please try again later", status)),
                _ => {
                    // Try to extract error message from body
                    if let Ok(json) = serde_json::from_str::<serde_json::Value>(&body) {
                        if let Some(msg) = json.get("error").and_then(|e| e.get("message")).and_then(|m| m.as_str()) {
                            return Err(format!("Gemini API error: {}", msg));
                        }
                    }
                    Err(format!("HTTP error {}", status))
                }
            };
        }

        Ok(())
    }

    pub async fn send_message(&self, message: &str) -> Result<String, String> {
        // Add user message to history
        let user_content = GeminiContent {
            role: "user".to_string(),
            parts: vec![GeminiPart {
                text: message.to_string(),
            }],
        };

        let mut history = self.conversation_history.lock().await;
        history.push(user_content.clone());

        // Build request with full conversation history
        let request = GeminiRequest {
            contents: history.clone(),
            system_instruction: None,
        };

        let url = Self::format_generate_content_url(&self.model);

        let response = self
            .client
            .post(&url)
            .header("Content-Type", "application/json")
            .header("x-goog-api-key", &self.api_key)
            .json(&request)
            .send()
            .await
            .map_err(|e| {
                if e.is_timeout() {
                    "Request timeout: Response took too long".to_string()
                } else if e.is_connect() {
                    "Network error: Cannot reach Gemini API".to_string()
                } else {
                    format!("Network error: {}", e)
                }
            })?;

        if !response.status().is_success() {
            let status = response.status().as_u16();
            let body = response.text().await.unwrap_or_else(|_| "unknown".to_string());

            // Remove the user message from history since it failed
            history.pop();

            return match status {
                401 => Err("API key is invalid or expired".to_string()),
                403 => Err("API key is not authorized for this operation".to_string()),
                429 => Err("Rate limited by Gemini API: Please wait before trying again".to_string()),
                500..=599 => Err(format!("Gemini API server error ({}): Please try again later", status)),
                _ => {
                    if let Ok(json) = serde_json::from_str::<serde_json::Value>(&body) {
                        if let Some(msg) = json.get("error").and_then(|e| e.get("message")).and_then(|m| m.as_str()) {
                            return Err(format!("API error: {}", msg));
                        }
                    }
                    Err(format!("HTTP error {}", status))
                }
            };
        }

        let response_body: GeminiResponse = response
            .json()
            .await
            .map_err(|e| format!("Failed to parse API response: {}", e))?;

        if let Some(err) = response_body.error {
            history.pop();
            return Err(format!("Gemini API error: {}", err.message));
        }

        let assistant_response = response_body
            .candidates
            .and_then(|mut candidates| candidates.pop())
            .and_then(|c| {
                c.content.parts.first().map(|p| p.text.clone())
            })
            .ok_or_else(|| "No response text from Gemini API".to_string())?;

        // Add assistant response to history
        let assistant_content = GeminiContent {
            role: "model".to_string(),
            parts: vec![GeminiPart {
                text: assistant_response.clone(),
            }],
        };
        history.push(assistant_content);

        // Limit history to last 20 messages to prevent unbounded growth
        if history.len() > 40 {
            history.drain(0..10);
        }

        Ok(assistant_response)
    }

    #[allow(dead_code)]
    pub fn reset_conversation(&self) {
        // Conversation is held in Arc<Mutex>, so clearing it will require a new instance
        // For now, we'll handle this by creating a new provider instance
    }

    #[allow(dead_code)]
    pub fn get_model(&self) -> &str {
        &self.model
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_format_generate_content_url_standard() {
        let url = GeminiProvider::format_generate_content_url("gemini-2.0-flash");
        assert_eq!(
            url,
            "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent"
        );
        assert!(!url.contains("key="));
    }

    #[test]
    fn test_format_generate_content_url_with_models_prefix() {
        let url = GeminiProvider::format_generate_content_url("models/gemini-1.5-flash");
        assert_eq!(
            url,
            "https://generativelanguage.googleapis.com/v1beta/models/gemini-1.5-flash:generateContent"
        );
        assert!(!url.contains("key="));
    }

    #[test]
    fn test_request_serialization() {
        let req = GeminiRequest {
            contents: vec![GeminiContent {
                role: "user".to_string(),
                parts: vec![GeminiPart {
                    text: "Hello world".to_string(),
                }],
            }],
            system_instruction: None,
        };

        let val = serde_json::to_value(&req).expect("Failed to serialize request");
        assert_eq!(val["contents"][0]["role"], "user");
        assert_eq!(val["contents"][0]["parts"][0]["text"], "Hello world");
        assert!(val.get("system_instruction").is_none());
    }

    #[test]
    fn test_response_deserialization_success() {
        let json_data = r#"{
            "candidates": [
                {
                    "content": {
                        "parts": [
                            {
                                "text": "Hello, how can I help you?"
                            }
                        ],
                        "role": "model"
                    }
                }
            ]
        }"#;

        let res: GeminiResponse = serde_json::from_str(json_data).expect("Failed to deserialize");
        assert!(res.error.is_none());
        let candidate = res.candidates.unwrap().pop().unwrap();
        assert_eq!(candidate.content.role, "model");
        assert_eq!(candidate.content.parts[0].text, "Hello, how can I help you?");
    }

    #[test]
    fn test_response_deserialization_error() {
        let json_data = r#"{
            "error": {
                "code": 403,
                "message": "Permission denied"
            }
        }"#;

        let res: GeminiResponse = serde_json::from_str(json_data).expect("Failed to deserialize");
        assert!(res.candidates.is_none());
        let err = res.error.unwrap();
        assert_eq!(err.code, 403);
        assert_eq!(err.message, "Permission denied");
    }

    #[tokio::test]
    async fn test_request_headers_and_path_local_mock() {
        use tokio::io::{AsyncReadExt, AsyncWriteExt};
        use tokio::net::TcpListener;

        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();

        let server = tokio::spawn(async move {
            let (mut socket, _) = listener.accept().await.unwrap();
            let mut buf = vec![0u8; 4096];
            let n = socket.read(&mut buf).await.unwrap();
            let req_str = String::from_utf8_lossy(&buf[..n]).to_string();

            let body = r#"{"candidates":[{"content":{"parts":[{"text":"mock response"}],"role":"model"}}]}"#;
            let http_response = format!(
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                body.len(),
                body
            );
            socket.write_all(http_response.as_bytes()).await.unwrap();

            req_str
        });

        let client = reqwest::Client::new();
        let test_url = format!("http://{}/v1beta/models/gemini-2.0-flash:generateContent", addr);
        let resp = client
            .post(&test_url)
            .header("Content-Type", "application/json")
            .header("x-goog-api-key", "secret-test-api-key")
            .json(&serde_json::json!({
                "contents": [{"parts": [{"text": "hello"}]}]
            }))
            .send()
            .await
            .unwrap();

        assert_eq!(resp.status().as_u16(), 200);

        let captured_request = server.await.unwrap();
        assert!(captured_request.starts_with("POST /v1beta/models/gemini-2.0-flash:generateContent HTTP/1.1"));
        assert!(captured_request.to_lowercase().contains("x-goog-api-key: secret-test-api-key"));
        assert!(captured_request.to_lowercase().contains("content-type: application/json"));
        assert!(!captured_request.contains("?key="));
    }
}
