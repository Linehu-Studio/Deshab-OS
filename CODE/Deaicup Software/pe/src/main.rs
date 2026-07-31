//! Deaicup Software — Windows PE64 entry point.
//!
//! The original application runs on eframe/egui. For the Deshab UTSM PE
//! compatibility layer we keep the egui UI logic but replace the eframe
//! runtime with a minimal hand-written Win32 backend:
//!   user32 window + message pump -> egui RawInput events
//!   egui tessellation            -> SoftRaster (pure Rust, raster.rs)
//!   32bpp DIB backbuffer         -> GDI BitBlt onto the window DC
//!
//! No CRT: the PE entry is `mainCRTStartup` (see .cargo/config.toml) and the
//! process ends via ExitProcess.

#![no_main]
#![allow(static_mut_refs)]

mod raster;
#[allow(dead_code)]
mod rt;
mod win32;

use egui::{self, Color32, Context, Event, Key, Modifiers, PointerButton, Pos2, Rect};

use raster::SoftRaster;
use win32::*;

/* ============================================================
 *  UI 逻辑（移植自原 eframe 应用 CODE/Deaicup Software/src）
 * ========================================================== */

#[derive(Debug, PartialEq, Clone, Copy, Default)]
enum Page {
    #[default]
    Home,
    Tool,
    Game,
    LinuxWare,
    WindowsWare,
    Set,
    About,
}

#[derive(Default)]
struct DeaicupSoftware {
    current_page: Page,
}

impl DeaicupSoftware {
    fn ui(&mut self, ctx: &Context) {
        // 原应用顶部是远程图片 https://deaicup.com/.../top.png。
        // PE 环境无网络，用标题栏替代，保留 75px 高度布局。
        egui::TopBottomPanel::top("top_image_panel").show(ctx, |ui| {
            ui.set_min_height(75.0);
            ui.vertical_centered(|ui| {
                ui.add_space(15.0);
                ui.label(egui::RichText::new("Deaicup Software").size(30.0).strong());
            });
        });

        // main
        egui::SidePanel::left("navigation_panel")
            .default_width(200.0)
            .resizable(true)
            .show(ctx, |ui| {
                ui.heading("Menu");
                ui.separator();
                ui.vertical(|ui| {
                    if ui.selectable_label(self.current_page == Page::Home, "HOME").clicked() {
                        self.current_page = Page::Home;
                    }
                    if ui.selectable_label(self.current_page == Page::Tool, "TOOL").clicked() {
                        self.current_page = Page::Tool;
                    }
                    if ui.selectable_label(self.current_page == Page::Game, "GAME").clicked() {
                        self.current_page = Page::Game;
                    }
                    if ui
                        .selectable_label(self.current_page == Page::LinuxWare, "LINUXWARE")
                        .clicked()
                    {
                        self.current_page = Page::LinuxWare;
                    }
                    if ui
                        .selectable_label(self.current_page == Page::WindowsWare, "WINDOWSWARE")
                        .clicked()
                    {
                        self.current_page = Page::WindowsWare;
                    }
                    if ui.selectable_label(self.current_page == Page::Set, "SET").clicked() {
                        self.current_page = Page::Set;
                    }
                    if ui
                        .selectable_label(self.current_page == Page::About, "ABOUT")
                        .clicked()
                    {
                        self.current_page = Page::About;
                    }
                })
            });
        egui::CentralPanel::default().show(ctx, |ui| {
            ui.separator();
            match self.current_page {
                Page::Home => ui.label("Home"),
                Page::Tool => ui.label("Tool"),
                Page::Game => ui.label("Game"),
                Page::LinuxWare => ui.label("LinuxWare"),
                Page::WindowsWare => ui.label("WindowsWare"),
                Page::Set => ui.label("Set"),
                Page::About => ui.label("About"),
            }
        });
    }
}

/* ============================================================
 *  全局输入状态（单线程：wnd_proc 只被 DispatchMessageW 同步调用）
 * ========================================================== */

static mut EVENTS: Vec<Event> = Vec::new();
static mut POINTER: Pos2 = Pos2::ZERO;
static mut MOD_ALT: bool = false;
static mut MOD_CTRL: bool = false;
static mut MOD_SHIFT: bool = false;
static mut QUIT: bool = false;

fn current_modifiers() -> Modifiers {
    unsafe {
        Modifiers {
            alt: MOD_ALT,
            ctrl: MOD_CTRL,
            shift: MOD_SHIFT,
            mac_cmd: false,
            command: MOD_CTRL,
        }
    }
}

#[inline]
fn lparam_x(l: LPARAM) -> i32 {
    (l as i32 & 0xFFFF) as i16 as i32
}
#[inline]
fn lparam_y(l: LPARAM) -> i32 {
    ((l as i32 >> 16) & 0xFFFF) as i16 as i32
}

fn push_pointer_button(button: PointerButton, pressed: bool, l: LPARAM) -> LRESULT {
    unsafe {
        POINTER = Pos2::new(lparam_x(l) as f32, lparam_y(l) as f32);
        EVENTS.push(Event::PointerButton {
            pos: POINTER,
            button,
            pressed,
            modifiers: current_modifiers(),
        });
    }
    0
}

fn vk_to_egui_key(vk: usize) -> Option<Key> {
    // 字符键不需要映射为 egui::Key —— 文本输入走 WM_CHAR -> Event::Text。
    Some(match vk {
        VK_LEFT => Key::ArrowLeft,
        VK_UP => Key::ArrowUp,
        VK_RIGHT => Key::ArrowRight,
        VK_DOWN => Key::ArrowDown,
        VK_BACK => Key::Backspace,
        VK_DELETE => Key::Delete,
        VK_RETURN => Key::Enter,
        VK_TAB => Key::Tab,
        VK_ESCAPE => Key::Escape,
        VK_SPACE => Key::Space,
        _ => return None,
    })
}

fn on_key(vk: usize, pressed: bool) -> LRESULT {
    unsafe {
        match vk {
            VK_SHIFT => MOD_SHIFT = pressed,
            VK_CONTROL => MOD_CTRL = pressed,
            VK_MENU => MOD_ALT = pressed,
            _ => {}
        }
        if let Some(key) = vk_to_egui_key(vk) {
            EVENTS.push(Event::Key {
                key,
                physical_key: None,
                pressed,
                repeat: false,
                modifiers: current_modifiers(),
            });
        }
    }
    0
}

extern "system" fn wnd_proc(hwnd: HWND, msg: u32, w: WPARAM, l: LPARAM) -> LRESULT {
    unsafe {
        match msg {
            WM_CLOSE | WM_DESTROY => {
                QUIT = true;
                PostQuitMessage(0);
                0
            }
            WM_MOUSEMOVE => {
                POINTER = Pos2::new(lparam_x(l) as f32, lparam_y(l) as f32);
                EVENTS.push(Event::PointerMoved(POINTER));
                0
            }
            WM_LBUTTONDOWN => push_pointer_button(PointerButton::Primary, true, l),
            WM_LBUTTONUP => push_pointer_button(PointerButton::Primary, false, l),
            WM_RBUTTONDOWN => push_pointer_button(PointerButton::Secondary, true, l),
            WM_RBUTTONUP => push_pointer_button(PointerButton::Secondary, false, l),
            WM_MBUTTONDOWN => push_pointer_button(PointerButton::Middle, true, l),
            WM_MBUTTONUP => push_pointer_button(PointerButton::Middle, false, l),
            WM_KEYDOWN => on_key(w, true),
            WM_KEYUP => on_key(w, false),
            WM_CHAR => {
                if let Some(c) = char::from_u32(w as u32) {
                    if c >= ' ' && c != '\x7f' {
                        EVENTS.push(Event::Text(c.to_string()));
                    }
                }
                0
            }
            WM_ERASEBKGND => 1, // 背景由 egui 全量绘制
            _ => DefWindowProcW(hwnd, msg, w, l),
        }
    }
}

/* ============================================================
 *  计时
 * ========================================================== */

fn now_secs() -> f64 {
    unsafe {
        static mut FREQ: i64 = 0;
        if FREQ == 0 {
            QueryPerformanceFrequency(&mut FREQ);
            if FREQ == 0 {
                FREQ = 1000;
            }
        }
        let mut c: i64 = 0;
        QueryPerformanceCounter(&mut c);
        c as f64 / FREQ as f64
    }
}

/* ============================================================
 *  PE 入口
 * ========================================================== */

/// ExitProcess wrapper: restore the kernel GS base before leaving.
fn exit_process(code: u32) -> ! {
    unsafe {
        rt::restore_tls();
        ExitProcess(code);
    }
}

#[no_mangle]
pub extern "C" fn mainCRTStartup() {
    unsafe {
        // Fake TEB/TLS must precede any std facility that might touch
        // thread locals (parking_lot, std::io, etc.).
        rt::install_fake_tls();
    }
    log("[deaicup] mainCRTStartup\n");

    unsafe {
        let hinst = GetModuleHandleW(core::ptr::null());

        let class_name = wide("DeaicupWindow");
        let wc = WNDCLASSEXW {
            cbSize: core::mem::size_of::<WNDCLASSEXW>() as u32,
            style: 0,
            lpfnWndProc: wnd_proc,
            cbClsExtra: 0,
            cbWndExtra: 0,
            hInstance: hinst,
            hIcon: 0,
            hCursor: LoadCursorW(0, IDC_ARROW),
            hbrBackground: 0,
            lpszMenuName: core::ptr::null(),
            lpszClassName: class_name.as_ptr(),
            hIconSm: 0,
        };
        RegisterClassExW(&wc);

        // 屏幕尺寸（shim 报 framebuffer 尺寸），窗口 800x600，超出则收缩
        let sw = GetSystemMetrics(SM_CXSCREEN);
        let sh = GetSystemMetrics(SM_CYSCREEN);
        let mut ww = 800;
        let mut wh = 600;
        if sw > 0 && ww > sw {
            ww = sw;
        }
        if sh > 0 && wh > sh {
            wh = sh;
        }

        let title = wide("Deaicup Software");
        let hwnd = CreateWindowExW(
            0,
            class_name.as_ptr(),
            title.as_ptr(),
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            ww,
            wh,
            0,
            0,
            hinst,
            core::ptr::null_mut(),
        );
        if hwnd == 0 {
            log("[deaicup] CreateWindowExW failed\n");
            exit_process(1);
        }
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        // 32bpp top-down DIB 后台缓冲
        let hdc = GetDC(hwnd);
        let memdc = CreateCompatibleDC(hdc);
        let mut bmi = BITMAPINFO::default();
        bmi.bmiHeader.biSize = core::mem::size_of::<BITMAPINFOHEADER>() as u32;
        bmi.bmiHeader.biWidth = ww;
        bmi.bmiHeader.biHeight = -wh; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        let mut bits: *mut core::ffi::c_void = core::ptr::null_mut();
        let hbmp = CreateDIBSection(memdc, &bmi, DIB_RGB_COLORS, &mut bits, 0, 0);
        if hbmp == 0 || bits.is_null() {
            log("[deaicup] CreateDIBSection failed\n");
            exit_process(1);
        }
        SelectObject(memdc, hbmp);
        log("[dc] SelectObject ok\n");

        /* 二分探针：逐项验证 std 设施，定位 Context::default 崩溃点 */
        {
            let mut v: Vec<u8> = Vec::with_capacity(16);
            for i in 0..64u8 { v.push(i); }
            log("[dc] vec ok\n");
            let s = String::from("hello");
            let _ = s.len();
            log("[dc] string ok\n");
            let mut h: std::collections::HashMap<u32, u32> = std::collections::HashMap::new();
            h.insert(1, 2); h.insert(3, 4);
            let _ = h.get(&1);
            log("[dc] hashmap ok\n");
            let a = std::sync::Arc::new(42u32);
            let b = a.clone();
            let _ = *b;
            log("[dc] arc ok\n");
            let m = std::sync::Mutex::new(7u32);
            *m.lock().unwrap() = 8;
            log("[dc] mutex ok\n");
        }

        let ctx = Context::default();
        log("[dc] Context::default ok\n");
        ctx.set_pixels_per_point(1.0);

        let mut app = DeaicupSoftware::default();
        let mut raster = SoftRaster::new();
        let mut msg = MSG::default();
        let mut frame: u64 = 0;
        log("[dc] app+raster ok\n");

        while !QUIT {
            // 消息泵：把 Win32 消息转成 egui 事件（由 wnd_proc 推入 EVENTS）
            while PeekMessageW(&mut msg, 0, 0, 0, PM_REMOVE) != 0 {
                if msg.message == WM_QUIT {
                    QUIT = true;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if QUIT {
                break;
            }
            if frame < 3 { log("[dc] msgpump ok\n"); }

            let events = core::mem::take(&mut *core::ptr::addr_of_mut!(EVENTS));
            let had_events = !events.is_empty();

            let input = egui::RawInput {
                screen_rect: Some(Rect::from_min_size(
                    Pos2::ZERO,
                    egui::vec2(ww as f32, wh as f32),
                )),
                time: Some(now_secs()),
                modifiers: current_modifiers(),
                events,
                ..Default::default()
            };
            if frame < 3 { log("[dc] rawinput ok\n"); }

            let full = ctx.run(input, |ctx| app.ui(&ctx));
            if frame < 3 { log("[dc] ctx.run ok\n"); }
            let repaint_after = full
                .viewport_output
                .values()
                .next()
                .map(|v| v.repaint_delay)
                .unwrap_or(std::time::Duration::from_millis(16));

            raster.apply_delta(&full.textures_delta);
            if frame < 3 { log("[dc] apply_delta ok\n"); }
            let prims = ctx.tessellate(full.shapes, full.pixels_per_point);
            if frame < 3 { log("[dc] tessellate ok\n"); }

            let dst = core::slice::from_raw_parts_mut(bits as *mut u32, (ww * wh) as usize);
            raster::clear(dst, Color32::from_rgb(0x30, 0x34, 0x3d));
            raster.paint(dst, ww, wh, &prims);
            if frame < 3 { log("[dc] paint ok\n"); }

            BitBlt(hdc, 0, 0, ww, wh, memdc, 0, 0, SRCCOPY);
            if frame < 3 { log("[dc] bitblt ok\n"); }

            frame += 1;
            if frame > 2 && !had_events && repaint_after > std::time::Duration::from_millis(5) {
                let ms = repaint_after.as_millis().min(50) as u32;
                Sleep(if ms == 0 { 5 } else { ms });
            }
        }

        log("[deaicup] quit, ExitProcess(0)\n");
        exit_process(0);
    }
}
