//! Minimal Win32 bindings used by the Deaicup PE backend.
//! Only the API subset that the Deshab UTSM PE shim implements is declared here.
//! All imports use raw-dylib so rust-lld can emit the import table without
//! any MSVC import libraries.

#![allow(non_snake_case, non_camel_case_types, dead_code)]

pub type HWND = isize;
pub type HDC = isize;
pub type HINSTANCE = isize;
pub type HICON = isize;
pub type HCURSOR = isize;
pub type HBRUSH = isize;
pub type HMENU = isize;
pub type HGDIOBJ = isize;
pub type HBITMAP = isize;
pub type HANDLE = isize;
pub type ATOM = u16;
pub type WPARAM = usize;
pub type LPARAM = isize;
pub type LRESULT = isize;
pub type BOOL = i32;
pub type UINT = u32;
pub type DWORD = u32;
pub type WORD = u16;
pub type LPCWSTR = *const u16;

pub const CW_USEDEFAULT: i32 = 0x80000000u32 as i32;

pub const WS_OVERLAPPED: u32 = 0x00000000;
pub const WS_CAPTION: u32 = 0x00C00000;
pub const WS_SYSMENU: u32 = 0x00080000;
pub const WS_THICKFRAME: u32 = 0x00040000;
pub const WS_MINIMIZEBOX: u32 = 0x00020000;
pub const WS_MAXIMIZEBOX: u32 = 0x00010000;
pub const WS_VISIBLE: u32 = 0x10000000;
pub const WS_OVERLAPPEDWINDOW: u32 = WS_OVERLAPPED
    | WS_CAPTION
    | WS_SYSMENU
    | WS_THICKFRAME
    | WS_MINIMIZEBOX
    | WS_MAXIMIZEBOX;

pub const SW_SHOW: i32 = 5;

pub const WM_CREATE: u32 = 0x0001;
pub const WM_DESTROY: u32 = 0x0002;
pub const WM_SIZE: u32 = 0x0005;
pub const WM_PAINT: u32 = 0x000F;
pub const WM_CLOSE: u32 = 0x0010;
pub const WM_QUIT: u32 = 0x0012;
pub const WM_ERASEBKGND: u32 = 0x0014;
pub const WM_KEYDOWN: u32 = 0x0100;
pub const WM_KEYUP: u32 = 0x0101;
pub const WM_CHAR: u32 = 0x0102;
pub const WM_TIMER: u32 = 0x0113;
pub const WM_MOUSEMOVE: u32 = 0x0200;
pub const WM_LBUTTONDOWN: u32 = 0x0201;
pub const WM_LBUTTONUP: u32 = 0x0202;
pub const WM_RBUTTONDOWN: u32 = 0x0204;
pub const WM_RBUTTONUP: u32 = 0x0205;
pub const WM_MBUTTONDOWN: u32 = 0x0207;
pub const WM_MBUTTONUP: u32 = 0x0208;

pub const PM_NOREMOVE: u32 = 0x0000;
pub const PM_REMOVE: u32 = 0x0001;

pub const VK_BACK: usize = 0x08;
pub const VK_TAB: usize = 0x09;
pub const VK_RETURN: usize = 0x0D;
pub const VK_SHIFT: usize = 0x10;
pub const VK_CONTROL: usize = 0x11;
pub const VK_MENU: usize = 0x12;
pub const VK_ESCAPE: usize = 0x1B;
pub const VK_SPACE: usize = 0x20;
pub const VK_LEFT: usize = 0x25;
pub const VK_UP: usize = 0x26;
pub const VK_RIGHT: usize = 0x27;
pub const VK_DOWN: usize = 0x28;
pub const VK_DELETE: usize = 0x2E;

pub const IDC_ARROW: usize = 32512;
pub const COLOR_WINDOW: u32 = 5;

pub const GWLP_USERDATA: i32 = -21;
pub const GWLP_WNDPROC: i32 = -4;

pub const SM_CXSCREEN: i32 = 0;
pub const SM_CYSCREEN: i32 = 1;

pub const SRCCOPY: u32 = 0x00CC0020;
pub const DIB_RGB_COLORS: u32 = 0;
pub const BI_RGB: u32 = 0;
pub const WHITE_BRUSH: i32 = 0;

pub const STD_OUTPUT_HANDLE: u32 = 0xFFFFFFF5;
pub const STD_ERROR_HANDLE: u32 = 0xFFFFFFF4;

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct POINT {
    pub x: i32,
    pub y: i32,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct RECT {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct MSG {
    pub hwnd: HWND,
    pub message: u32,
    pub wParam: WPARAM,
    pub lParam: LPARAM,
    pub time: u32,
    pub pt: POINT,
}

#[repr(C)]
pub struct WNDCLASSEXW {
    pub cbSize: UINT,
    pub style: UINT,
    pub lpfnWndProc: WNDPROC,
    pub cbClsExtra: i32,
    pub cbWndExtra: i32,
    pub hInstance: HINSTANCE,
    pub hIcon: HICON,
    pub hCursor: HCURSOR,
    pub hbrBackground: HBRUSH,
    pub lpszMenuName: LPCWSTR,
    pub lpszClassName: LPCWSTR,
    pub hIconSm: HICON,
}

pub type WNDPROC = extern "system" fn(HWND, u32, WPARAM, LPARAM) -> LRESULT;

#[repr(C)]
pub struct PAINTSTRUCT {
    pub hdc: HDC,
    pub fErase: BOOL,
    pub rcPaint: RECT,
    pub fRestore: BOOL,
    pub fIncUpdate: BOOL,
    pub rgbReserved: [u8; 32],
}

impl Default for PAINTSTRUCT {
    fn default() -> Self {
        unsafe { core::mem::zeroed() }
    }
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct BITMAPINFOHEADER {
    pub biSize: u32,
    pub biWidth: i32,
    pub biHeight: i32,
    pub biPlanes: u16,
    pub biBitCount: u16,
    pub biCompression: u32,
    pub biSizeImage: u32,
    pub biXPelsPerMeter: i32,
    pub biYPelsPerMeter: i32,
    pub biClrUsed: u32,
    pub biClrImportant: u32,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct RGBQUAD {
    pub rgbBlue: u8,
    pub rgbGreen: u8,
    pub rgbRed: u8,
    pub rgbReserved: u8,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct BITMAPINFO {
    pub bmiHeader: BITMAPINFOHEADER,
    pub bmiColors: [RGBQUAD; 1],
}

#[link(name = "user32", kind = "raw-dylib")]
extern "system" {
    pub fn RegisterClassExW(lpwcx: *const WNDCLASSEXW) -> ATOM;
    pub fn CreateWindowExW(
        dwExStyle: DWORD,
        lpClassName: LPCWSTR,
        lpWindowName: LPCWSTR,
        dwStyle: DWORD,
        x: i32,
        y: i32,
        nWidth: i32,
        nHeight: i32,
        hWndParent: HWND,
        hMenu: HMENU,
        hInstance: HINSTANCE,
        lpParam: *mut core::ffi::c_void,
    ) -> HWND;
    pub fn DestroyWindow(hWnd: HWND) -> BOOL;
    pub fn ShowWindow(hWnd: HWND, nCmdShow: i32) -> BOOL;
    pub fn UpdateWindow(hWnd: HWND) -> BOOL;
    pub fn GetMessageW(lpMsg: *mut MSG, hWnd: HWND, wMsgFilterMin: u32, wMsgFilterMax: u32) -> BOOL;
    pub fn PeekMessageW(
        lpMsg: *mut MSG,
        hWnd: HWND,
        wMsgFilterMin: u32,
        wMsgFilterMax: u32,
        wRemoveMsg: UINT,
    ) -> BOOL;
    pub fn TranslateMessage(lpMsg: *const MSG) -> BOOL;
    pub fn DispatchMessageW(lpMsg: *const MSG) -> LRESULT;
    pub fn PostQuitMessage(nExitCode: i32);
    pub fn DefWindowProcW(hWnd: HWND, msg: u32, wParam: WPARAM, lParam: LPARAM) -> LRESULT;
    pub fn GetClientRect(hWnd: HWND, lpRect: *mut RECT) -> BOOL;
    pub fn InvalidateRect(hWnd: HWND, lpRect: *const RECT, bErase: BOOL) -> BOOL;
    pub fn BeginPaint(hWnd: HWND, lpPaint: *mut PAINTSTRUCT) -> HDC;
    pub fn EndPaint(hWnd: HWND, lpPaint: *const PAINTSTRUCT) -> BOOL;
    pub fn GetDC(hWnd: HWND) -> HDC;
    pub fn ReleaseDC(hWnd: HWND, hDC: HDC) -> i32;
    pub fn LoadCursorW(hInstance: HINSTANCE, lpCursorName: usize) -> HCURSOR;
    pub fn SetCursor(hCursor: HCURSOR) -> HCURSOR;
    pub fn GetCursorPos(lpPoint: *mut POINT) -> BOOL;
    pub fn ScreenToClient(hWnd: HWND, lpPoint: *mut POINT) -> BOOL;
    pub fn GetAsyncKeyState(vKey: i32) -> i16;
    pub fn SetTimer(hWnd: HWND, nIDEvent: usize, uElapse: UINT, lpTimerFunc: usize) -> usize;
    pub fn KillTimer(hWnd: HWND, uIDEvent: usize) -> BOOL;
    pub fn MessageBeep(uType: UINT) -> BOOL;
    pub fn GetSystemMetrics(nIndex: i32) -> i32;
}

#[link(name = "gdi32", kind = "raw-dylib")]
extern "system" {
    pub fn CreateCompatibleDC(hdc: HDC) -> HDC;
    pub fn CreateDIBSection(
        hdc: HDC,
        pbmi: *const BITMAPINFO,
        usage: UINT,
        ppvBits: *mut *mut core::ffi::c_void,
        hSection: HANDLE,
        offset: DWORD,
    ) -> HBITMAP;
    pub fn SelectObject(hdc: HDC, h: HGDIOBJ) -> HGDIOBJ;
    pub fn DeleteObject(ho: HGDIOBJ) -> BOOL;
    pub fn DeleteDC(hdc: HDC) -> BOOL;
    pub fn BitBlt(
        hdc: HDC,
        x: i32,
        y: i32,
        cx: i32,
        cy: i32,
        hdcSrc: HDC,
        x1: i32,
        y1: i32,
        rop: DWORD,
    ) -> BOOL;
    pub fn GetStockObject(i: i32) -> HGDIOBJ;
    pub fn SetDIBitsToDevice(
        hdc: HDC,
        xDest: i32,
        yDest: i32,
        w: DWORD,
        h: DWORD,
        xSrc: i32,
        ySrc: i32,
        startScan: UINT,
        numLines: UINT,
        lpvBits: *const core::ffi::c_void,
        lpbmi: *const BITMAPINFO,
        colorUse: UINT,
    ) -> i32;
    pub fn GdiFlush() -> BOOL;
}

#[link(name = "kernel32", kind = "raw-dylib")]
extern "system" {
    pub fn GetModuleHandleW(lpModuleName: LPCWSTR) -> HINSTANCE;
    pub fn GetStdHandle(nStdHandle: DWORD) -> HANDLE;
    pub fn WriteFile(
        hFile: HANDLE,
        lpBuffer: *const u8,
        nNumberOfBytesToWrite: DWORD,
        lpNumberOfBytesWritten: *mut DWORD,
        lpOverlapped: *mut core::ffi::c_void,
    ) -> BOOL;
    pub fn ExitProcess(uExitCode: UINT) -> !;
    pub fn GetCommandLineA() -> *const u8;
    pub fn GetTickCount() -> DWORD;
    pub fn QueryPerformanceCounter(lpPerformanceCount: *mut i64) -> BOOL;
    pub fn QueryPerformanceFrequency(lpFrequency: *mut i64) -> BOOL;
    pub fn Sleep(dwMilliseconds: DWORD);
}

/// UTF-16 helper: build a null-terminated wide string.
pub fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(core::iter::once(0)).collect()
}

/// Debug log through the shim's stdout (serial).
pub fn log(s: &str) {
    unsafe {
        let h = GetStdHandle(STD_OUTPUT_HANDLE);
        let mut written: DWORD = 0;
        WriteFile(h, s.as_ptr(), s.len() as DWORD, &mut written, core::ptr::null_mut());
    }
}
