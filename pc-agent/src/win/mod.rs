//! Windows bits: single instance, run at logon, opening URLs, COM init, network category, Wi-Fi name, firewall
//! rule for the relay, local IP. No PowerShell anywhere: endpoint protection (Sophos Intercept X
//! "BrowserAncestorPowershell") blocks powershell.exe when the agent was started by an installer downloaded in a
//! browser. COM (Network List Manager), IP Helper and netsh instead.

pub mod net;

use windows::Win32::Foundation::{ERROR_ALREADY_EXISTS, GetLastError, HANDLE};
use windows::Win32::System::Com::{COINIT_MULTITHREADED, CoInitializeEx};
use windows::Win32::System::Threading::{CreateMutexW, GetCurrentProcess, SetProcessWorkingSetSize};
use windows::Win32::UI::Shell::ShellExecuteW;
use windows::Win32::UI::WindowsAndMessaging::{SW_HIDE, SW_SHOWNORMAL};
use windows::core::{HSTRING, PCWSTR, w};

pub use net::{allow_relay_on_this_network, local_ip_towards, network_category, wifi_name};

/// Process creation flag: no console window for child processes (netsh, yt-dlp, ffmpeg).
pub const CREATE_NO_WINDOW: u32 = 0x0800_0000;

const RUN_KEY: &str = r"Software\Microsoft\Windows\CurrentVersion\Run";
const RUN_NAME: &str = "MeowBox Agent";

/// True if this is the only running agent. Same mutex name as the Python agent, so 0.2.x and 0.3 exclude each other.
/// The handle is never closed: the mutex stays held until the process exits.
pub fn single_instance() -> bool {
    // SAFETY: plain Win32 calls.
    unsafe {
        match CreateMutexW(None, false, w!("Local\\MeowBoxAgent")) {
            Ok(HANDLE(_)) => GetLastError() != ERROR_ALREADY_EXISTS,
            Err(_) => true, // cannot tell: run rather than refuse to start
        }
    }
}

/// Open a URL in the default browser, or a folder in Explorer.
pub fn open(target: &str) {
    let t = HSTRING::from(target);
    // SAFETY: all strings outlive the call.
    unsafe {
        ShellExecuteW(None, w!("open"), &t, PCWSTR::null(), PCWSTR::null(), SW_SHOWNORMAL);
    }
}

/// Run a program elevated (UAC prompt), hidden. True if it was started.
pub fn run_elevated(exe: &str, args: &str) -> bool {
    let (e, a) = (HSTRING::from(exe), HSTRING::from(args));
    // SAFETY: all strings outlive the call.
    let r = unsafe { ShellExecuteW(None, w!("runas"), &e, &a, PCWSTR::null(), SW_HIDE) };
    r.0 as isize > 32
}

/// Give the pages the dialog's OpenGL driver touched back to Windows after the window closed (the driver keeps its
/// heaps for the life of the process; they are paged back in only if a dialog opens again).
pub fn trim_memory() {
    // SAFETY: (usize::MAX, usize::MAX) asks Windows to trim the current process's working set.
    unsafe {
        let _ = SetProcessWorkingSetSize(GetCurrentProcess(), usize::MAX, usize::MAX);
    }
}

/// COM for this thread (multithreaded apartment); harmless if already initialised.
pub fn com_init() {
    // SAFETY: may be called any number of times per thread; a mode mismatch just returns an error we ignore.
    unsafe {
        let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
    }
}

/// This PC's name as the speaker shows it.
pub fn pc_name() -> String {
    std::env::var("COMPUTERNAME").ok().filter(|s| !s.is_empty()).unwrap_or_else(|| "PC".into())
}

/// The tray exe: this exe if it is MeowBoxAgent.exe, else MeowBoxAgent.exe next to it.
pub fn tray_exe() -> std::path::PathBuf {
    let me = std::env::current_exe().unwrap_or_default();
    if me.file_stem().is_some_and(|s| s.eq_ignore_ascii_case("MeowBoxAgent")) {
        me
    } else {
        me.with_file_name("MeowBoxAgent.exe")
    }
}

fn run_key(write: bool) -> std::io::Result<winreg::RegKey> {
    let hkcu = winreg::RegKey::predef(winreg::enums::HKEY_CURRENT_USER);
    if write {
        hkcu.open_subkey_with_flags(RUN_KEY, winreg::enums::KEY_SET_VALUE | winreg::enums::KEY_QUERY_VALUE)
    } else {
        hkcu.open_subkey(RUN_KEY)
    }
}

pub fn autostart_enabled() -> bool {
    run_key(false).and_then(|k| k.get_raw_value(RUN_NAME)).is_ok()
}

/// Same value as the installer writes: the quoted tray exe path.
pub fn set_autostart(on: bool) -> std::io::Result<()> {
    let k = run_key(true)?;
    if on {
        k.set_value(RUN_NAME, &format!("\"{}\"", tray_exe().display()))
    } else {
        match k.delete_value(RUN_NAME) {
            Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(e),
            _ => Ok(()),
        }
    }
}
