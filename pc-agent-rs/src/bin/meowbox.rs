//! meowbox.exe: the command line (console). Phase 0: version and config only; the other commands come in phase 1.

use meowbox_agent::config::Config;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("--version") => println!("{}", meowbox_agent::VERSION),
        Some("tray") => {
            // kept for old shortcuts: start the tray exe next to this one
            let exe = std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.join("MeowBoxAgent.exe")));
            match exe.map(std::process::Command::new).map(|mut c| c.spawn()) {
                Some(Ok(_)) => {}
                _ => {
                    eprintln!("MeowBoxAgent.exe not found next to meowbox.exe");
                    std::process::exit(1);
                }
            }
        }
        Some("config") => {
            let p = meowbox_agent::config_path();
            let c = Config::load(&p);
            println!(
                "{}\n{}",
                p.display(),
                serde_json::to_string_pretty(&serde_json::Value::Object(c.to_map())).unwrap()
            );
        }
        _ => {
            eprintln!("MeowBox PC agent {} (Rust, phase 0): only --version, tray, config", meowbox_agent::VERSION);
            std::process::exit(2);
        }
    }
}
