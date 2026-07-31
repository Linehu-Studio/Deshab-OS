//! Minimal CRT / libm runtime for the no-CRT PE64 build.
//!
//! The UTSM PE shim only resolves DLL imports (kernel32/user32/gdi32).
//! Symbols below are *link-time* references emitted by LLVM/std, so they must
//! live inside the PE image itself:
//!   - string intrinsics: memcpy / memmove / memset / memcmp
//!   - MSVC runtime:      __chkstk, _fltused, _tls_index, _tls_used,
//!                        __CxxFrameHandler3 (never called, panic = abort)
//!   - f32 libm:          floorf/ceilf/roundf, sinf/cosf, atan2f, powf,
//!                        exp2f, acosf, cbrtf, _hypotf
//!
//! Math routines are ported from musl-libm (MIT) where accuracy matters
//! (sin/cos/atan) and use simple polynomial/series elsewhere. Visual use by
//! egui only needs ~1e-5 relative accuracy.
//!
//! TLS: Rust std on windows-msvc checks `_tls_used` and (if nonzero) accesses
//! thread locals through gs:[0x58] -> slots[_tls_index]. We are Ring0 with no
//! TEB, so mainCRTStartup installs a fake GS base pointing at a zeroed TLS
//! block (install_fake_tls) and restores it before ExitProcess.

#![allow(non_snake_case)]

use core::arch::asm;

/* ============================================================
 *  string intrinsics
 * ========================================================== */

/// # Safety
/// Standard C contract.
#[no_mangle]
pub unsafe extern "C" fn memcpy(dst: *mut u8, src: *const u8, n: usize) -> *mut u8 {
    let orig = dst;
    asm!(
        "rep movsb",
        inout("rcx") n => _,
        inout("rsi") src => _,
        inout("rdi") dst => _,
        options(nostack, preserves_flags)
    );
    orig
}

/// # Safety
/// Standard C contract.
#[no_mangle]
pub unsafe extern "C" fn memmove(dst: *mut u8, src: *const u8, n: usize) -> *mut u8 {
    let d = dst as usize;
    let s = src as usize;
    if d < s || d >= s.wrapping_add(n) {
        memcpy(dst, src, n)
    } else {
        // Backward copy without touching DF (ABI requires DF=0).
        let mut i = n;
        while i > 0 {
            i -= 1;
            core::ptr::write_volatile(dst.add(i), core::ptr::read_volatile(src.add(i)));
        }
        dst
    }
}

/// # Safety
/// Standard C contract.
#[no_mangle]
pub unsafe extern "C" fn memset(dst: *mut u8, val: i32, n: usize) -> *mut u8 {
    let orig = dst;
    asm!(
        "rep stosb",
        inout("rcx") n => _,
        inout("rdi") dst => _,
        in("al") val as u8,
        options(nostack, preserves_flags)
    );
    orig
}

/// # Safety
/// Standard C contract.
#[no_mangle]
pub unsafe extern "C" fn memcmp(a: *const u8, b: *const u8, n: usize) -> i32 {
    // Volatile reads keep LLVM from idiom-recognizing this back into memcmp.
    for i in 0..n {
        let x = core::ptr::read_volatile(a.add(i));
        let y = core::ptr::read_volatile(b.add(i));
        if x != y {
            return x as i32 - y as i32;
        }
    }
    0
}

/* ============================================================
 *  MSVC runtime bits
 * ========================================================== */

/// Stack probe. Our stack is one large committed region (see /STACK flag),
/// so probing pages is unnecessary. An empty body compiles to `ret`,
/// which trivially satisfies the preserve-everything contract.
#[no_mangle]
pub extern "C" fn __chkstk() {}

/// Referenced by std's unwind tables (.pdata personality). With
/// panic = "abort" it is never invoked; loop loudly if it ever is.
#[no_mangle]
pub extern "C" fn __CxxFrameHandler3() -> ! {
    loop {
        core::hint::spin_loop();
    }
}

/// CRT float-usage marker (data symbol only).
#[no_mangle]
pub static _fltused: i32 = 0x9875;

/// TLS slot index of this module. We use slot 0 of the fake TLS array.
#[no_mangle]
pub static _tls_index: u32 = 0;

/// IMAGE_TLS_DIRECTORY64 stand-in (all zeros: no raw template, no callbacks).
/// std only checks that its address is nonzero to decide TLS is supported.
#[no_mangle]
pub static _tls_used: [u8; 0x30] = [0; 0x30];

/* ---- fake TEB / TLS so std thread_local access can't fault ---- */

const IA32_GS_BASE: u32 = 0xC000_0101;

static mut OLD_GS_BASE: u64 = 0;
static mut TLS_READY: bool = false;

fn rdmsr(msr: u32) -> u64 {
    let lo: u32;
    let hi: u32;
    unsafe {
        asm!("rdmsr", in("ecx") msr, out("eax") lo, out("edx") hi,
             options(nomem, nostack, preserves_flags));
    }
    ((hi as u64) << 32) | lo as u64
}

fn wrmsr(msr: u32, val: u64) {
    unsafe {
        asm!("wrmsr", in("ecx") msr, in("eax") val as u32,
             in("edx") (val >> 32) as u32, options(nomem, nostack, preserves_flags));
    }
}

/// Point GS at a minimal fake TEB whose ThreadLocalStoragePointer (0x58)
/// references a zeroed slots array; slot 0 covers `_tls_index == 0`.
/// All const-init thread locals in std are zero-valid (None/0).
///
/// # Safety
/// Ring0 only; call once at PE entry before touching std facilities.
pub unsafe fn install_fake_tls() {
    static mut TEB: [u8; 0x100] = [0; 0x100];
    static mut SLOTS: [usize; 64] = [0; 64];
    static mut TLS_BLOCK: [u8; 8192] = [0; 8192];

    if TLS_READY {
        return;
    }
    let teb = core::ptr::addr_of_mut!(TEB);
    let slots = core::ptr::addr_of_mut!(SLOTS);
    let block = core::ptr::addr_of_mut!(TLS_BLOCK);
    (*slots)[0] = block as usize;
    *((*teb).as_mut_ptr().add(0x58) as *mut usize) = slots as usize;

    OLD_GS_BASE = rdmsr(IA32_GS_BASE);
    wrmsr(IA32_GS_BASE, teb as u64);
    TLS_READY = true;
}

/// Restore the kernel's GS base (call before ExitProcess).
///
/// # Safety
/// Ring0 only; pair with install_fake_tls.
pub unsafe fn restore_tls() {
    if TLS_READY {
        wrmsr(IA32_GS_BASE, OLD_GS_BASE);
        TLS_READY = false;
    }
}

/* ============================================================
 *  f32 libm
 * ========================================================== */

#[inline(always)]
fn asu32(x: f32) -> u32 {
    x.to_bits()
}
#[inline(always)]
fn asf32(x: u32) -> f32 {
    f32::from_bits(x)
}

#[no_mangle]
pub extern "C" fn floorf(x: f32) -> f32 {
    // |x| >= 2^23 is already integral; NaN passes the compare false and
    // degrades to 0.0, which egui never feeds us anyway.
    if x.abs() >= 8388608.0 {
        return x;
    }
    let i = x as i32; // truncate toward zero
    let f = i as f32;
    if f > x {
        f - 1.0
    } else {
        f
    }
}

#[no_mangle]
pub extern "C" fn ceilf(x: f32) -> f32 {
    if x.abs() >= 8388608.0 {
        return x;
    }
    let i = x as i32;
    let f = i as f32;
    if f < x {
        f + 1.0
    } else {
        f
    }
}

/// C semantics: round half away from zero.
#[no_mangle]
pub extern "C" fn roundf(x: f32) -> f32 {
    if x >= 0.0 {
        floorf(x + 0.5)
    } else {
        ceilf(x - 0.5)
    }
}

#[no_mangle]
pub extern "C" fn truncf(x: f32) -> f32 {
    if x.abs() >= 8388608.0 {
        return x;
    }
    x as i32 as f32
}

/// f64 truncation toward zero (LLVM libcall on SSE2-only targets).
#[no_mangle]
pub extern "C" fn trunc(x: f64) -> f64 {
    // 2^52: every f64 with |x| >= 2^52 is already integral.
    if x.abs() >= 4503599627370496.0 {
        return x;
    }
    x as i64 as f64
}

/* ---- sin/cos: musl __sindf/__cosdf + medium Cody-Waite reduction ---- */

// |sin(x)/x - s(x)| < 2**-27.5 on the reduced range.
const S1: f64 = -0.166666666416265235595;
const S2: f64 = 0.0083333293858894631756;
const S3: f64 = -0.000198393348360966317347;
const S4: f64 = 0.0000027183114939898219064;

const C0: f64 = -0.499999997251031003120;
const C1: f64 = 0.0416666233237390631894;
const C2: f64 = -0.00138867637746099294692;
const C3: f64 = 0.0000243904487962774090654;

const PIO2_HI: f64 = 1.57079625129699707031e+00;
const PIO2_LO: f64 = 7.54978941586159635335e-08;
const INV_PIO2: f64 = 6.36619772367581382433e-01; // 2/pi
const TOINT: f64 = 1.5 / f64::EPSILON;
const TWO_PI: f64 = 6.28318530717958647693;

#[inline]
fn sindf(x: f64) -> f64 {
    let z = x * x;
    let w = z * z;
    let r = S3 + z * S4;
    let s = z * x;
    (x + s * (S1 + z * S2)) + s * w * r
}

#[inline]
fn cosdf(x: f64) -> f64 {
    let z = x * x;
    let w = z * z;
    let r = C2 + z * C3;
    ((1.0 + z * C0) + w * C1) + (w * z) * r
}

/// Reduce x modulo pi/2 (Cody-Waite, double precision). Returns (n, y)
/// with y = x - n*pi/2 accurate for moderate |x|.
#[inline]
fn rem_pio2_medium(x: f64) -> (i32, f64) {
    // Coarse pre-reduction keeps the accurate path valid for any angle egui
    // can produce (ellipse/bezier angles are already tiny).
    let x = if x.abs() > 105.0 {
        x - (x / TWO_PI).trunc() * TWO_PI
    } else {
        x
    };
    let fn_ = x * INV_PIO2 + TOINT - TOINT;
    let n = fn_ as i32;
    (n, x - fn_ * PIO2_HI - fn_ * PIO2_LO)
}

#[no_mangle]
pub extern "C" fn sinf(x: f32) -> f32 {
    if x.abs() < 1.0e-30 {
        return x;
    }
    let (n, y) = rem_pio2_medium(x as f64);
    (match n & 3 {
        0 => sindf(y),
        1 => cosdf(y),
        2 => sindf(-y),
        _ => -cosdf(y),
    }) as f32
}

#[no_mangle]
pub extern "C" fn cosf(x: f32) -> f32 {
    let (n, y) = rem_pio2_medium(x as f64);
    (match n & 3 {
        0 => cosdf(y),
        1 => sindf(-y),
        2 => -cosdf(y),
        _ => sindf(y),
    }) as f32
}

/* ---- atan2f: musl atanf + quadrant fix ---- */

const ATAN_HI: [f32; 4] = [
    4.6364760399e-01,
    7.8539812565e-01,
    9.8279371262e-01,
    1.5707962513e+00,
];
const ATAN_LO: [f32; 4] = [
    8.8023035407e-08,
    1.2093354704e-08,
    1.2587686365e-08,
    7.5497894159e-08,
];
const AT: [f32; 5] = [
    3.3333328366e-01,
    -1.9999158382e-01,
    1.4253635705e-01,
    -1.0648017377e-01,
    6.1687607318e-02,
];
const PI_F: f32 = 3.1415927410e+00;

fn atanf(x: f32) -> f32 {
    let mut x = x;
    let ix = asu32(x);
    let sign = ix >> 31;
    let aix = ix & 0x7fffffff;
    if aix >= 0x4c800000 {
        // |x| >= 2^26
        if x.is_nan() {
            return x;
        }
        let z = ATAN_HI[3] + 7.5231638e-37;
        return if sign != 0 { -z } else { z };
    }
    let id: i32;
    if aix < 0x3ee00000 {
        // |x| < 0.4375
        if aix < 0x39800000 {
            return x; // |x| < 2^-12: atan(x) ~ x
        }
        id = -1;
    } else {
        x = x.abs();
        if aix < 0x3f980000 {
            if aix < 0x3f300000 {
                id = 0;
                x = (2.0 * x - 1.0) / (2.0 + x);
            } else {
                id = 1;
                x = (x - 1.0) / (x + 1.0);
            }
        } else if aix < 0x401c0000 {
            id = 2;
            x = (x - 1.5) / (1.0 + 1.5 * x);
        } else {
            id = 3;
            x = -1.0 / x;
        }
    }
    let z = x * x;
    let w = z * z;
    let s1 = z * (AT[0] + w * (AT[2] + w * AT[4]));
    let s2 = w * (AT[1] + w * AT[3]);
    if id < 0 {
        return x - x * (s1 + s2);
    }
    let i = id as usize;
    let z2 = ATAN_HI[i] - ((x * (s1 + s2) - ATAN_LO[i]) - x);
    if sign != 0 {
        -z2
    } else {
        z2
    }
}

#[no_mangle]
pub extern "C" fn atan2f(y: f32, x: f32) -> f32 {
    if y == 0.0 {
        return if x < 0.0 { PI_F } else { y }; // crude, matches C for our uses
    }
    if x == 0.0 {
        return if y < 0.0 { -PI_F / 2.0 } else { PI_F / 2.0 };
    }
    let ax = x.abs();
    let ay = y.abs();
    let z = if ax > ay {
        atanf(ay / ax)
    } else {
        PI_F / 2.0 - atanf(ax / ay)
    };
    if x < 0.0 {
        if y < 0.0 {
            z - PI_F
        } else {
            PI_F - z
        }
    } else if y < 0.0 {
        -z
    } else {
        z
    }
}

/* ---- exp2f / log2f / powf ---- */

/// ldexp-style scaling by 2^n (n clamped to f32 range by callers).
fn scalbnf(mut r: f32, mut n: i32) -> f32 {
    while n > 127 {
        r *= asf32(0x7F00_0000); // 2^127
        n -= 127;
    }
    while n < -126 {
        r *= asf32(0x0080_0000); // 2^-126
        n += 126;
    }
    r * asf32(((127 + n) as u32) << 23)
}

#[no_mangle]
pub extern "C" fn exp2f(x: f32) -> f32 {
    if x > 128.0 {
        return f32::INFINITY;
    }
    if x < -150.0 {
        return 0.0;
    }
    // n = round-to-nearest-even(x) via the magic-number trick.
    let t = x + 12582912.0; // 1.5 * 2^23
    let nf = t - 12582912.0;
    let n = nf as i32;
    let f = x - nf; // in [-0.5, 0.5]
    // 2^f: Taylor in ln2, degree 6 -> ~1e-10 rel error on [-0.5, 0.5].
    let p = 1.0
        + f * (0.6931471805599453
            + f * (0.24022650695910070
                + f * (0.05550410866482158
                    + f * (0.009618129107628477
                        + f * (0.0013333558146428443 + f * 0.00015403530393381609)))));
    scalbnf(p, n)
}

/// log2(x) for x > 0; accuracy ~1e-7 via atanh series in double.
fn log2f(x: f32) -> f32 {
    let ix = asu32(x);
    let e = ((ix >> 23) as i32) - 127;
    let m = asf32((ix & 0x007F_FFFF) | 0x3F80_0000); // [1, 2)
    let t = ((m - 1.0) / (m + 1.0)) as f64;
    let t2 = t * t;
    let mut sum = 0.0f64;
    let mut term = t;
    for k in 0..8 {
        sum += term / (2 * k + 1) as f64;
        term *= t2;
    }
    let ln_m = 2.0 * sum;
    (e as f64 + ln_m * 1.4426950408889634) as f32
}

#[no_mangle]
pub extern "C" fn powf(x: f32, y: f32) -> f32 {
    if y == 0.0 {
        return 1.0;
    }
    if x == 1.0 {
        return 1.0;
    }
    if x == 0.0 {
        return 0.0;
    }
    if x == 2.0 {
        return exp2f(y);
    }
    if x < 0.0 {
        let yi = y as i32;
        if yi as f32 == y {
            let r = exp2f(y * log2f(-x));
            return if yi & 1 != 0 { -r } else { r };
        }
        return f32::NAN;
    }
    exp2f(y * log2f(x))
}

/* ---- acosf: musl ---- */

const PIO2_HI_F: f32 = 1.5707962513e+00;
const PIO2_LO_F: f32 = 7.5497894159e-08;
const PS0: f32 = 1.6666586697e-01;
const PS1: f32 = -4.2743422091e-02;
const PS2: f32 = -8.6563630030e-03;
const QS1: f32 = -7.0662963390e-01;

#[inline]
fn acos_r(z: f32) -> f32 {
    z * (PS0 + z * (PS1 + z * PS2)) / (1.0 + z * QS1)
}

#[no_mangle]
pub extern "C" fn acosf(x: f32) -> f32 {
    let hx = asu32(x);
    let ix = hx & 0x7fffffff;
    if ix >= 0x3f800000 {
        // |x| >= 1
        if ix == 0x3f800000 {
            if hx >> 31 != 0 {
                return 2.0 * PIO2_HI_F; // acos(-1) = pi
            }
            return 0.0; // acos(1) = 0
        }
        return f32::NAN;
    }
    if ix < 0x3f000000 {
        // |x| < 0.5
        if ix <= 0x32800000 {
            return PIO2_HI_F; // |x| < 2^-26
        }
        return PIO2_HI_F - (x - (PIO2_LO_F - x * acos_r(x * x)));
    }
    if hx >> 31 != 0 {
        // x < -0.5
        let z = (1.0 + x) * 0.5;
        let s = z.sqrt();
        let w = acos_r(z) * s - PIO2_LO_F;
        return 2.0 * PIO2_HI_F - 2.0 * (s + w);
    }
    // x > 0.5
    let z = (1.0 - x) * 0.5;
    let s = z.sqrt();
    let df = asf32(asu32(s) & 0xFFFFF000); // keep df*df rounding-exact
    let c = (z - df * df) / (s + df);
    let w = acos_r(z) * s + c;
    2.0 * (df + w)
}

/* ---- cbrtf / hypotf ---- */

#[no_mangle]
pub extern "C" fn cbrtf(x: f32) -> f32 {
    if x == 0.0 {
        return x;
    }
    let r = exp2f(log2f(x.abs()) / 3.0);
    if x < 0.0 {
        -r
    } else {
        r
    }
}

#[no_mangle]
pub extern "C" fn _hypotf(x: f32, y: f32) -> f32 {
    let ax = x.abs();
    let ay = y.abs();
    let (a, b) = if ax > ay { (ax, ay) } else { (ay, ax) };
    if a == 0.0 {
        return 0.0;
    }
    let r = b / a;
    a * (1.0 + r * r).sqrt()
}

/// Plain alias in case LLVM references the C99 spelling.
#[no_mangle]
pub extern "C" fn hypotf(x: f32, y: f32) -> f32 {
    _hypotf(x, y)
}
