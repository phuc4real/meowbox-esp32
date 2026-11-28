//! MeowBoxAgent.exe: the tray app (windowed, no console).
#![windows_subsystem = "windows"]

use meowbox_agent::config::{Config, SharedConfig};
use meowbox_agent::link::{CoreHandle, Options};
use meowbox_agent::{state::SharedState, ui::TrayApp};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let verbose = args.iter().any(|a| a == "-v" || a == "--verbose");
    let selftest: u32 = args
        .iter()
        .position(|a| a == "--selftest-dialog")
        .and_then(|i| args.get(i + 1))
        .and_then(|n| n.parse().ok())
        .unwrap_or(0);

    // --dev: run next to an installed agent (no single-instance check); --open: open the window at once
    let dev = args.iter().any(|a| a == "--dev");
    let open = args.iter().any(|a| a == "--open");
    // screenshots (with --dev): --theme <id>, --dark / --light, --page settings
    let value = |k: &str| args.iter().position(|a| a == k).and_then(|i| args.get(i + 1)).cloned();
    let dev_look = meowbox_agent::ui::DevLook {
        theme: value("--theme").filter(|_| dev),
        dark: if !dev {
            None
        } else if args.iter().any(|a| a == "--dark") {
            Some(true)
        } else if args.iter().any(|a| a == "--light") {
            Some(false)
        } else {
            None
        },
    };
    let page = match value("--page").as_deref() {
        Some("settings") => meowbox_agent::ui::Page::Settings,
        _ => meowbox_agent::ui::Page::Connect,
    };

    let dir = meowbox_agent::app_dir();
    meowbox_agent::log::setup(verbose, true, &dir);
    let cfg_path = meowbox_agent::config_path();
    // the self-test may run next to an installed agent
    if selftest == 0 && !dev && !meowbox_agent::win::single_instance() {
        // second launch (Start menu, or the installer's finish page): bring up the running agent's window
        if !meowbox_agent::ui::tray::open_running() {
            let h = Config::load(&cfg_path).known_host().to_owned();
            if !h.is_empty() {
                meowbox_agent::win::open(&format!("http://{h}/"));
            }
        }
        return;
    }
    tracing::info!("MeowBox Agent {} starting (tray)", meowbox_agent::VERSION);
    let core = CoreHandle::start(
        SharedConfig::load(cfg_path),
        SharedState::default(),
        Options { update_check: selftest == 0, ..Options::default() },
    );
    let mut app = match TrayApp::new(core) {
        Ok(a) => a,
        Err(e) => {
            tracing::error!("tray: {e}");
            return;
        }
    };
    app.selftest_reopen(selftest);
    app.set_dev_look(dev_look);
    if open {
        app.open_page(page);
    }
    app.run();
    if selftest > 0 {
        tracing::info!("selftest: dialog opened {} times", app.dialogs_opened);
    }
}
