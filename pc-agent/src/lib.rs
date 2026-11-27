//! MeowBox PC agent (Rust rewrite, docs/rewrite_pc_agent.md).
//!
//! Same files and protocol as the Python agent 0.2.x: `%APPDATA%\MeowBox\agent.json`, `agent.log`, `hook.key`,
//! `%LOCALAPPDATA%\MeowBox\bin`, and `/ws/pc` protocol 2.

pub mod cli;
pub mod coding;
pub mod config;
pub mod discovery;
pub mod gpu;
pub mod json;
pub mod link;
pub mod log;
pub mod media;
pub mod relay;
pub mod speaker_api;
pub mod state;
pub mod stats;
pub mod tools;
pub mod tts;
pub mod ui;
pub mod update;
pub mod win;

use std::path::PathBuf;

pub const VERSION: &str = env!("CARGO_PKG_VERSION");
/// Speaker <-> agent protocol, see MB_PROTO in firmware/components/net/include/net.h (2: token in hello).
pub const PROTO: u32 = 2;
pub const REPO: &str = "phuc4real/meowbox-esp32";

fn env_dir(var: &str) -> PathBuf {
    std::env::var_os(var)
        .map(PathBuf::from)
        .unwrap_or_else(|| std::env::var_os("USERPROFILE").map(PathBuf::from).unwrap_or_default())
}

/// `%APPDATA%\MeowBox`: agent.json, agent.log, hook.key.
pub fn app_dir() -> PathBuf {
    env_dir("APPDATA").join("MeowBox")
}

/// `%APPDATA%\MeowBox\agent.json`.
pub fn config_path() -> PathBuf {
    app_dir().join("agent.json")
}

/// `%LOCALAPPDATA%\MeowBox\bin`: yt-dlp and ffmpeg.
pub fn bin_dir() -> PathBuf {
    env_dir("LOCALAPPDATA").join("MeowBox").join("bin")
}
