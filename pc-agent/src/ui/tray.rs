//! The tray icon, straight on Shell_NotifyIconW: a hidden window receives the icon's mouse messages and wake-ups
//! from other threads. The window procedure only queues events; the app drains the queue from its loop (or from
//! the dialog's frame while a dialog is open), so nothing re-enters the app while a menu or dialog runs.
//!
//! Own code instead of the tray-icon crate: that crate has no balloon/toast, which the agent uses for short
//! messages ("downloading ffmpeg", "token wrong", ...). The menu is built fresh each time it opens, like pystray's.

use std::cell::RefCell;
use std::collections::VecDeque;

use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, POINT, WPARAM};
use windows::Win32::Graphics::Gdi::{
    BI_RGB, BITMAPINFO, BITMAPINFOHEADER, CreateBitmap, CreateDIBSection, DIB_RGB_COLORS, DeleteObject, GetDC,
    ReleaseDC,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::Shell::{
    NIF_ICON, NIF_INFO, NIF_MESSAGE, NIF_TIP, NIIF_INFO, NIM_ADD, NIM_DELETE, NIM_MODIFY, NOTIFYICONDATAW,
    Shell_NotifyIconW,
};
use windows::Win32::UI::WindowsAndMessaging::*;
use windows::core::{PCWSTR, w};

const WM_TRAY: u32 = WM_APP + 1;
const WM_WAKE: u32 = WM_APP + 2;
/// Posted to ourselves so the main loop's GetMessage returns and drains the event queue (see `push_sent`).
const WM_DRAIN: u32 = WM_APP + 3;
const ICON_ID: u32 = 1;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TrayEvent {
    /// Left click on the icon (the menu's default action).
    Click,
    /// Right click: open the menu at this screen point.
    Menu(i32, i32),
    /// Another thread has news (state changed, toast queued).
    Wake,
    /// Explorer restarted: the icon must be added again.
    TaskbarCreated,
}

thread_local! {
    static EVENTS: RefCell<VecDeque<TrayEvent>> = const { RefCell::new(VecDeque::new()) };
    /// Set while a dialog is open, so a queued event repaints it (its frame drains the queue).
    static DIALOG_CTX: RefCell<Option<eframe::egui::Context>> = const { RefCell::new(None) };
    static TASKBAR_CREATED: std::cell::Cell<u32> = const { std::cell::Cell::new(0) };
}

fn push(ev: TrayEvent) {
    EVENTS.with(|q| q.borrow_mut().push_back(ev));
    DIALOG_CTX.with(|c| {
        if let Some(ctx) = c.borrow().as_ref() {
            ctx.request_repaint();
        }
    });
}

/// For events that arrive as *sent* messages: Explorer delivers the icon's mouse messages and TaskbarCreated with
/// SendNotifyMessage, which runs the window procedure inside GetMessage without making GetMessage return. Without a
/// posted message after it, the main loop would not see the event until something else is posted (a state change),
/// and once the speaker is connected nothing is: clicks on the icon did nothing (0.3.0).
fn push_sent(hwnd: HWND, ev: TrayEvent) {
    push(ev);
    // SAFETY: posting to our own window.
    let _ = unsafe { PostMessageW(Some(hwnd), WM_DRAIN, WPARAM(0), LPARAM(0)) };
}

/// Next queued event (main thread only).
pub fn pop() -> Option<TrayEvent> {
    EVENTS.with(|q| q.borrow_mut().pop_front())
}

pub fn set_dialog_ctx(ctx: Option<eframe::egui::Context>) {
    DIALOG_CTX.with(|c| *c.borrow_mut() = ctx);
}

extern "system" fn wndproc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        WM_TRAY => {
            match lp.0 as u32 {
                WM_LBUTTONUP => push_sent(hwnd, TrayEvent::Click),
                WM_RBUTTONUP | WM_CONTEXTMENU => {
                    let mut pt = POINT::default();
                    // SAFETY: pt is a valid out pointer.
                    let _ = unsafe { GetCursorPos(&mut pt) };
                    push_sent(hwnd, TrayEvent::Menu(pt.x, pt.y));
                }
                _ => {}
            }
            LRESULT(0)
        }
        WM_WAKE => {
            push(TrayEvent::Wake);
            LRESULT(0)
        }
        WM_DRAIN => LRESULT(0), // only there to make GetMessage return

        m if m != 0 && m == TASKBAR_CREATED.with(|c| c.get()) => {
            push_sent(hwnd, TrayEvent::TaskbarCreated);
            LRESULT(0)
        }
        // SAFETY: default handling for everything else.
        _ => unsafe { DefWindowProcW(hwnd, msg, wp, lp) },
    }
}

/// Wakes the main thread from any thread (posts to the tray window).
#[derive(Clone, Copy)]
pub struct Waker(isize);

impl Waker {
    pub fn wake(&self) {
        // SAFETY: posting to a window handle is safe from any thread; a stale handle just fails.
        let _ = unsafe { PostMessageW(Some(HWND(self.0 as *mut _)), WM_WAKE, WPARAM(0), LPARAM(0)) };
    }
}

fn wide<const N: usize>(s: &str) -> [u16; N] {
    let mut out = [0u16; N];
    for (i, c) in s.encode_utf16().take(N - 1).enumerate() {
        out[i] = c;
    }
    out
}

/// HICON from straight RGBA.
pub fn make_icon(rgba: &[u8], size: u32) -> HICON {
    // SAFETY: the DIB section is size×size×4 bytes and we write exactly that; GDI objects are freed after
    // CreateIconIndirect copied them.
    unsafe {
        let bmi = BITMAPINFO {
            bmiHeader: BITMAPINFOHEADER {
                biSize: size_of::<BITMAPINFOHEADER>() as u32,
                biWidth: size as i32,
                biHeight: -(size as i32), // top-down
                biPlanes: 1,
                biBitCount: 32,
                biCompression: BI_RGB.0,
                ..Default::default()
            },
            ..Default::default()
        };
        let dc = GetDC(None);
        let mut bits: *mut core::ffi::c_void = std::ptr::null_mut();
        let color = CreateDIBSection(Some(dc), &bmi, DIB_RGB_COLORS, &mut bits, None, 0).unwrap_or_default();
        ReleaseDC(None, dc);
        if !bits.is_null() {
            let dst = std::slice::from_raw_parts_mut(bits as *mut u8, (size * size * 4) as usize);
            for (d, s) in dst.as_chunks_mut::<4>().0.iter_mut().zip(rgba.as_chunks::<4>().0) {
                *d = [s[2], s[1], s[0], s[3]]; // BGRA
            }
        }
        let mask = CreateBitmap(size as i32, size as i32, 1, 1, None);
        let info = ICONINFO { fIcon: true.into(), hbmMask: mask, hbmColor: color, ..Default::default() };
        let icon = CreateIconIndirect(&info).unwrap_or_default();
        let _ = DeleteObject(color.into());
        let _ = DeleteObject(mask.into());
        icon
    }
}

/// Icon size the tray uses at the current DPI (16 at 100 %, 24 at 150 %, 32 at 200 %).
pub fn tray_icon_size() -> u32 {
    // SAFETY: plain metric query.
    let n = unsafe { GetSystemMetrics(SM_CXSMICON) };
    if n > 0 { n as u32 } else { 16 }
}

/// The hidden window that receives the icon's messages (and wake-ups from other threads). Main thread only: the
/// window belongs to the calling thread.
fn create_window() -> windows::core::Result<HWND> {
    // SAFETY: standard window class registration and creation; the class name is a static string.
    unsafe {
        let inst = GetModuleHandleW(None)?;
        let class = w!("MeowBoxAgentTray");
        let wc = WNDCLASSW {
            lpfnWndProc: Some(wndproc),
            hInstance: inst.into(),
            lpszClassName: class,
            ..Default::default()
        };
        RegisterClassW(&wc);
        TASKBAR_CREATED.with(|c| c.set(RegisterWindowMessageW(w!("TaskbarCreated"))));
        CreateWindowExW(
            WINDOW_EX_STYLE(0),
            class,
            w!("MeowBox Agent"),
            WS_OVERLAPPED,
            0,
            0,
            0,
            0,
            None,
            None,
            Some(inst.into()),
            None,
        )
    }
}

pub struct Tray {
    hwnd: HWND,
    icon: HICON,
    tip: String,
}

impl Tray {
    /// Create the hidden window and add the icon. Main thread only (the window belongs to the calling thread).
    pub fn new(icon: HICON, tip: &str) -> windows::core::Result<Self> {
        let hwnd = create_window()?;
        let tray = Self { hwnd, icon, tip: tip.to_owned() };
        tray.add();
        Ok(tray)
    }

    fn data(&self) -> NOTIFYICONDATAW {
        NOTIFYICONDATAW {
            cbSize: size_of::<NOTIFYICONDATAW>() as u32,
            hWnd: self.hwnd,
            uID: ICON_ID,
            uFlags: NIF_MESSAGE | NIF_ICON | NIF_TIP,
            uCallbackMessage: WM_TRAY,
            hIcon: self.icon,
            szTip: wide(&self.tip),
            ..Default::default()
        }
    }

    /// (Re)add the icon, e.g. after Explorer restarted.
    pub fn add(&self) {
        // SAFETY: data() is a fully initialised NOTIFYICONDATAW.
        unsafe {
            let _ = Shell_NotifyIconW(NIM_ADD, &self.data());
        }
    }

    pub fn waker(&self) -> Waker {
        Waker(self.hwnd.0 as isize)
    }

    pub fn set(&mut self, icon: HICON, tip: &str) {
        let old = std::mem::replace(&mut self.icon, icon);
        self.tip = tip.chars().take(127).collect();
        // SAFETY: as in add(); the old icon is no longer referenced by the shell after NIM_MODIFY.
        unsafe {
            let _ = Shell_NotifyIconW(NIM_MODIFY, &self.data());
            if old != icon && !old.is_invalid() {
                let _ = DestroyIcon(old);
            }
        }
    }

    /// Balloon / toast next to the tray ("MeowBox" title), like pystray's Icon.notify.
    pub fn notify(&self, text: &str, title: &str) {
        let mut d = self.data();
        d.uFlags = NIF_INFO;
        d.szInfo = wide(text);
        d.szInfoTitle = wide(title);
        d.dwInfoFlags = NIIF_INFO;
        // SAFETY: as in add().
        unsafe {
            let _ = Shell_NotifyIconW(NIM_MODIFY, &d);
        }
    }

    /// Show a popup menu at (x, y) and wait for the choice: the chosen item's id, or None.
    pub fn popup(&self, items: &[MenuItem], x: i32, y: i32) -> Option<u32> {
        // SAFETY: the menu lives for this call only; strings outlive AppendMenuW (which copies them).
        unsafe {
            let menu = CreatePopupMenu().ok()?;
            for it in items {
                match it {
                    MenuItem::Separator => {
                        let _ = AppendMenuW(menu, MF_SEPARATOR, 0, PCWSTR::null());
                    }
                    MenuItem::Item { id, text, enabled, checked, default } => {
                        let mut f = MF_STRING;
                        if !enabled {
                            f |= MF_GRAYED;
                        }
                        if *checked {
                            f |= MF_CHECKED;
                        }
                        let t = windows::core::HSTRING::from(text.as_str());
                        let _ = AppendMenuW(menu, f, *id as usize, &t);
                        if *default {
                            let _ = SetMenuDefaultItem(menu, *id, 0);
                        }
                    }
                }
            }
            // the menu closes when clicking elsewhere only if our window is in the foreground
            let _ = SetForegroundWindow(self.hwnd);
            let r = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, None, self.hwnd, None);
            let _ = PostMessageW(Some(self.hwnd), WM_NULL, WPARAM(0), LPARAM(0));
            let _ = DestroyMenu(menu);
            (r.0 > 0).then_some(r.0 as u32)
        }
    }
}

impl Drop for Tray {
    fn drop(&mut self) {
        // SAFETY: removing our own icon and window.
        unsafe {
            let _ = Shell_NotifyIconW(NIM_DELETE, &self.data());
            let _ = DestroyIcon(self.icon);
            let _ = DestroyWindow(self.hwnd);
        }
    }
}

pub enum MenuItem {
    Separator,
    Item { id: u32, text: String, enabled: bool, checked: bool, default: bool },
}

impl MenuItem {
    pub fn item(id: u32, text: impl Into<String>) -> Self {
        Self::Item { id, text: text.into(), enabled: true, checked: false, default: false }
    }
    pub fn label(text: impl Into<String>) -> Self {
        Self::Item { id: 0, text: text.into(), enabled: false, checked: false, default: false }
    }
    pub fn check(id: u32, text: impl Into<String>, checked: bool) -> Self {
        Self::Item { id, text: text.into(), enabled: true, checked, default: false }
    }
    pub fn default(id: u32, text: impl Into<String>) -> Self {
        Self::Item { id, text: text.into(), enabled: true, checked: false, default: true }
    }
}

/// Run one message (blocking). False when WM_QUIT arrived.
pub fn pump_one() -> bool {
    let mut msg = MSG::default();
    // SAFETY: standard message loop on the thread that owns the tray window.
    unsafe {
        let r = GetMessageW(&mut msg, None, 0, 0).0;
        if r <= 0 {
            tracing::debug!("message loop ends (GetMessage {r}, message 0x{:x})", msg.message);
            return false;
        }
        let _ = TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, Instant};
    use windows::Win32::System::Threading::GetCurrentThreadId;

    /// Explorer sends the icon's clicks with SendNotifyMessage: the main loop must see them at once, not only when
    /// the next posted message comes (the 0.3.0 "tray icon stops answering once connected" bug).
    #[test]
    fn a_click_sent_like_explorer_reaches_the_loop() {
        let hwnd = create_window().expect("window");
        let h = hwnd.0 as isize;
        // SAFETY: plain thread id query.
        let me = unsafe { GetCurrentThreadId() };
        std::thread::spawn(move || {
            // SAFETY: sending to a window of another thread; posting WM_QUIT to a thread id.
            unsafe {
                let _ = SendNotifyMessageW(HWND(h as *mut _), WM_TRAY, WPARAM(1), LPARAM(WM_RBUTTONUP as isize));
                std::thread::sleep(Duration::from_secs(3));
                // watchdog: without the fix the loop would wait forever
                let _ = PostThreadMessageW(me, WM_QUIT, WPARAM(0), LPARAM(0));
            }
        });
        let t = Instant::now();
        let mut got = None;
        while got.is_none() && pump_one() {
            got = pop();
        }
        assert!(matches!(got, Some(TrayEvent::Menu(_, _))), "no event: {got:?}");
        assert!(t.elapsed() < Duration::from_secs(1), "event came only after {:?}", t.elapsed());
        // SAFETY: our own window.
        let _ = unsafe { DestroyWindow(hwnd) };
    }
}
