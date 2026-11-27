//! Windows bits. Phase 0: single instance and opening URLs / folders. The rest (autostart, network category, Wi-Fi
//! name, firewall rule, local IP) comes with the core in phase 1.

use windows::Win32::Foundation::{ERROR_ALREADY_EXISTS, GetLastError, HANDLE};
use windows::Win32::System::Threading::{CreateMutexW, GetCurrentProcess, SetProcessWorkingSetSize};
use windows::Win32::UI::Shell::ShellExecuteW;
use windows::Win32::UI::WindowsAndMessaging::SW_SHOWNORMAL;
use windows::core::{HSTRING, PCWSTR, w};

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

/// Give the pages the dialog's OpenGL driver touched back to Windows after the window closed (the driver keeps its
/// heaps for the life of the process; they are paged back in only if a dialog opens again).
pub fn trim_memory() {
    // SAFETY: (usize::MAX, usize::MAX) asks Windows to trim the current process's working set.
    unsafe {
        let _ = SetProcessWorkingSetSize(GetCurrentProcess(), usize::MAX, usize::MAX);
    }
}
