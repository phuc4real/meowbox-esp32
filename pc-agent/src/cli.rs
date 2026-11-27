//! Command line (meowbox.exe), as cli.py: run / tray / notify / say / wifi-setup / tools / coding / config.
//! With no command it runs the headless agent; options without a command also mean `run` (the old
//! `agent.py --no-relay` habit).

use crate::config::{Config, SharedConfig};
use crate::state::SharedState;
use clap::{Parser, Subcommand, ValueEnum};
use std::path::PathBuf;

#[derive(Parser, Debug)]
#[command(name = "meowbox", about = format!("MeowBox PC agent {}", crate::VERSION), version = crate::VERSION)]
pub struct Cli {
    /// file cấu hình (mặc định %APPDATA%\MeowBox\agent.json)
    #[arg(long, global = true)]
    pub config: Option<PathBuf>,
    /// IP[:port] của loa (bỏ qua tìm tự động)
    #[arg(long, global = true)]
    pub host: Option<String>,
    #[arg(short, long, global = true)]
    pub verbose: bool,
    #[command(subcommand)]
    pub cmd: Option<Cmd>,
}

#[derive(Subcommand, Debug)]
pub enum Cmd {
    /// icon ở khay hệ thống (mặc định của MeowBoxAgent.exe)
    Tray,
    /// chạy agent không có giao diện
    Run {
        /// không mở YouTube relay
        #[arg(long)]
        no_relay: bool,
        /// không cập nhật yt-dlp
        #[arg(long)]
        no_update: bool,
    },
    /// gửi thông báo
    Notify {
        title: String,
        #[arg(default_value = "")]
        text: String,
        #[arg(long, value_enum, default_value = "chime")]
        sound: Sound,
        #[arg(long, value_enum, default_value = "normal")]
        priority: Priority,
        #[arg(long, default_value_t = 10)]
        duration: i64,
    },
    /// đọc câu bằng TTS Windows và gửi lên loa
    Say {
        text: String,
        /// một phần tên/ngôn ngữ giọng, vd 'An' hoặc 'vi-VN'
        #[arg(long)]
        voice: Option<String>,
        /// chỉ lưu WAV ra file, không gửi
        #[arg(long)]
        save: Option<PathBuf>,
    },
    /// bật/tắt Wi-Fi cài đặt MeowBox-XXXX trên loa (tự tắt sau 10 phút không ai vào)
    #[command(name = "wifi-setup")]
    WifiSetup {
        #[arg(value_enum, default_value = "on")]
        action: OnOff,
    },
    /// yt-dlp dùng cho YouTube relay: xem phiên bản / tải bản mới nhất
    Tools {
        #[arg(value_enum, default_value = "status")]
        action: ToolsAction,
    },
    /// Meow Coding: bật/gỡ hook Claude Code để mèo trên loa hiện phiên đang code
    Coding {
        #[arg(value_enum, default_value = "status")]
        action: CodingAction,
    },
    /// xem/sửa cấu hình
    Config {
        /// IP[:port] loa (IP hiện trên tile Hệ thống của loa)
        #[arg(long = "set-host", value_name = "HOST")]
        set_host: Option<String>,
        /// chỉ nối nếu loa ở IP đó có tên này, vd MeowBox-1A2B ('' = loa nào cũng được)
        #[arg(long)]
        speaker: Option<String>,
        #[arg(long)]
        token: Option<String>,
        #[arg(long)]
        relay_port: Option<u16>,
    },
}

#[derive(ValueEnum, Clone, Copy, Debug)]
pub enum Sound {
    Chime,
    Alert,
    None,
}

#[derive(ValueEnum, Clone, Copy, Debug)]
pub enum Priority {
    Low,
    Normal,
    High,
}

#[derive(ValueEnum, Clone, Copy, Debug, PartialEq)]
pub enum OnOff {
    On,
    Off,
    Status,
}

#[derive(ValueEnum, Clone, Copy, Debug)]
pub enum ToolsAction {
    Status,
    Update,
}

#[derive(ValueEnum, Clone, Copy, Debug)]
pub enum CodingAction {
    Install,
    Uninstall,
    Status,
}

fn enum_name<T: ValueEnum>(v: T) -> String {
    v.to_possible_value().map(|p| p.get_name().to_owned()).unwrap_or_default()
}

const SUBCOMMANDS: &[&str] = &["tray", "run", "notify", "say", "wifi-setup", "tools", "coding", "config", "help"];

/// Insert `run` where the command would be when none is given (global options may come first).
pub fn with_default_command(argv: Vec<String>) -> Vec<String> {
    let mut i = 1;
    while i < argv.len() {
        let a = argv[i].as_str();
        if SUBCOMMANDS.contains(&a) || a == "-h" || a == "--help" || a == "-V" || a == "--version" {
            return argv;
        }
        if (a == "--config" || a == "--host") && i + 1 < argv.len() {
            i += 2;
            continue;
        }
        if a == "-v" || a == "--verbose" || a.starts_with("--config=") || a.starts_with("--host=") {
            i += 1;
            continue;
        }
        break; // first token that is neither a global option nor a command: `run` goes here
    }
    let mut out = argv;
    out.insert(i.min(out.len()), "run".into());
    out
}

fn fail(msg: impl std::fmt::Display, code: i32) -> ! {
    eprintln!("{msg}");
    std::process::exit(code)
}

async fn speaker_host(cfg: &Config) -> String {
    match crate::discovery::find_speaker(cfg, |_| {}).await {
        Some(sp) => sp.host,
        None => fail("Không nối được loa. Dùng --host IP hoặc: meowbox config --set-host IP", 1),
    }
}

async fn post(cfg: &Config, path: &str, body: Vec<u8>, ctype: &str) {
    let host = speaker_host(cfg).await;
    match crate::speaker_api::speaker_request(&host, path, &cfg.token, Some((body, ctype))).await {
        Ok((code, text)) => {
            println!("{code} {text}");
            if code >= 400 {
                std::process::exit(1);
            }
        }
        Err(e) => fail(format!("Lỗi kết nối: {e}"), 1),
    }
}

fn rt() -> tokio::runtime::Runtime {
    tokio::runtime::Builder::new_current_thread().enable_all().build().expect("tokio runtime")
}

pub fn main() {
    let cli = Cli::parse_from(with_default_command(std::env::args().collect()));
    crate::log::setup(cli.verbose, false, &crate::app_dir());
    let path = cli.config.clone().unwrap_or_else(crate::config_path);
    let mut cfg = Config::load(&path);
    let cmd = cli.cmd.unwrap_or(Cmd::Run { no_relay: false, no_update: false });

    match cmd {
        Cmd::Tray => {
            let exe = crate::win::tray_exe();
            if let Err(e) = std::process::Command::new(&exe).spawn() {
                fail(format!("{}: {e}", exe.display()), 1);
            }
            return;
        }
        Cmd::Config { set_host, speaker, token, relay_port } => {
            if let Some(v) = set_host {
                cfg.host = v;
            }
            if let Some(v) = speaker {
                cfg.speaker = v;
            }
            if let Some(v) = token {
                cfg.token = v;
            }
            if let Some(v) = relay_port {
                cfg.relay_port = v;
            }
            if let Err(e) = cfg.save(&path) {
                fail(format!("{}: {e}", path.display()), 1);
            }
            let text = serde_json::to_string_pretty(&serde_json::Value::Object(cfg.to_map())).unwrap_or_default();
            println!("{}\n{text}", path.display());
            return;
        }
        Cmd::Coding { action } => {
            let port = cfg.coding_port;
            let r = match action {
                CodingAction::Install => crate::coding::hooks::install(port),
                CodingAction::Uninstall => crate::coding::hooks::uninstall(),
                CodingAction::Status => Ok(()),
            };
            if let Err(e) = r {
                fail(format!("{}: {e}", crate::coding::claude_settings().display()), 1);
            }
            let on = if crate::coding::hooks::hooks_installed() { "đã bật" } else { "chưa bật" };
            println!(
                "{}: hook MeowBox {on} (http://127.0.0.1:{port}/hook)",
                crate::coding::claude_settings().display()
            );
            return;
        }
        Cmd::Tools { action } => {
            rt().block_on(async {
                let dir = crate::tools::ytdlp::root();
                match action {
                    ToolsAction::Update => match crate::tools::ytdlp::update(|step| println!("… {step}")).await {
                        Ok(v) => println!("yt-dlp {v} -> {}", dir.display()),
                        Err(e) => fail(format!("yt-dlp: {e}"), 1),
                    },
                    ToolsAction::Status => {
                        let v = crate::tools::ytdlp::version().unwrap_or_else(|| "chưa cài".into());
                        let latest = crate::tools::ytdlp::latest().await.unwrap_or_else(|| "?".into());
                        println!("yt-dlp: {v} ({}), mới nhất trên GitHub: {latest}", dir.display());
                        let ff = crate::tools::ffmpeg::status();
                        match ff.path {
                            Some(p) => println!(
                                "ffmpeg: {} ({})",
                                crate::tools::ffmpeg::version().unwrap_or_default(),
                                p.display()
                            ),
                            None => println!("ffmpeg: chưa có (tự tải khi phát YouTube live lần đầu)"),
                        }
                    }
                }
            });
            return;
        }
        _ => {}
    }
    if let Some(h) = cli.host {
        cfg.host = h;
        cfg.last_host.clear();
        cfg.speaker.clear();
    }

    match cmd {
        Cmd::Run { no_relay, no_update } => {
            tracing::info!("MeowBox Agent {} starting (headless)", crate::VERSION);
            let opts = crate::link::Options { relay: !no_relay, ytdlp_update: !no_update, ..Default::default() };
            crate::link::run_forever(SharedConfig::new(cfg, path), SharedState::default(), opts);
        }
        Cmd::Notify { title, text, sound, priority, duration } => {
            let body = serde_json::json!({"title": title, "text": text, "sound": enum_name(sound),
                "priority": enum_name(priority), "duration": duration});
            rt().block_on(post(&cfg, "/api/notify", body.to_string().into_bytes(), "application/json"));
        }
        Cmd::WifiSetup { action } => rt().block_on(async {
            if action == OnOff::Status {
                let host = speaker_host(&cfg).await;
                match crate::speaker_api::speaker_request(&host, "/api/wifi/portal", &cfg.token, None).await {
                    Ok((code, text)) => println!("{code} {text}"),
                    Err(e) => fail(format!("Lỗi kết nối: {e}"), 1),
                }
            } else {
                let body = serde_json::json!({"on": action == OnOff::On}).to_string().into_bytes();
                post(&cfg, "/api/wifi/portal", body, "application/json").await;
            }
        }),
        Cmd::Say { text, voice, save } => rt().block_on(async {
            crate::win::com_init();
            let wav = match crate::tts::tts_wav(&text, voice.as_deref()).await {
                Ok(w) => w,
                Err(e) => fail(format!("TTS: {e}"), 1),
            };
            if let Some(p) = save {
                if let Err(e) = std::fs::write(&p, &wav) {
                    fail(format!("{}: {e}", p.display()), 1);
                }
                println!("saved {} bytes -> {}", wav.len(), p.display());
            } else {
                let title: String = text.chars().take(60).collect();
                let q: String = url_encode(&title);
                post(&cfg, &format!("/api/notify/audio?title={q}"), wav, "audio/wav").await;
            }
        }),
        _ => unreachable!(),
    }
}

/// application/x-www-form-urlencoded value (Python's urlencode: space → +).
pub fn url_encode(s: &str) -> String {
    let mut out = String::new();
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => out.push(b as char),
            b' ' => out.push('+'),
            _ => out.push_str(&format!("%{b:02X}")),
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v(a: &[&str]) -> Vec<String> {
        a.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn default_command_is_run() {
        assert_eq!(with_default_command(v(&["meowbox"])), v(&["meowbox", "run"]));
        assert_eq!(with_default_command(v(&["meowbox", "--no-relay"])), v(&["meowbox", "run", "--no-relay"]));
        assert_eq!(
            with_default_command(v(&["meowbox", "--host", "1.2.3.4", "--no-relay"])),
            v(&["meowbox", "--host", "1.2.3.4", "run", "--no-relay"])
        );
        assert_eq!(with_default_command(v(&["meowbox", "-v", "notify", "hi"])), v(&["meowbox", "-v", "notify", "hi"]));
        assert_eq!(with_default_command(v(&["meowbox", "--version"])), v(&["meowbox", "--version"]));
    }

    #[test]
    fn parses_like_argparse() {
        let c = Cli::parse_from(v(&["meowbox", "notify", "Build done", "--sound", "alert", "--host", "1.2.3.4"]));
        assert_eq!(c.host.as_deref(), Some("1.2.3.4"));
        match c.cmd {
            Some(Cmd::Notify { title, text, sound, priority, duration }) => {
                assert_eq!((title.as_str(), text.as_str(), duration), ("Build done", "", 10));
                assert_eq!((enum_name(sound), enum_name(priority)), ("alert".into(), "normal".into()));
            }
            other => panic!("{other:?}"),
        }
        let c = Cli::parse_from(v(&["meowbox", "wifi-setup"]));
        assert!(matches!(c.cmd, Some(Cmd::WifiSetup { action: OnOff::On })));
        let c = Cli::parse_from(v(&["meowbox", "config", "--set-host", "1.2.3.4", "--relay-port", "9000"]));
        assert!(matches!(c.cmd, Some(Cmd::Config { relay_port: Some(9000), .. })));
    }

    #[test]
    fn url_encoding() {
        assert_eq!(url_encode("Xin chào bạn"), "Xin+ch%C3%A0o+b%E1%BA%A1n");
    }
}
