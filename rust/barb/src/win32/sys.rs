//! The Win32 calls barb makes that std has no way to make: the console's raw input, overlapped
//! named pipes, processes started suspended in a job of their own with only the handles meant for
//! them inherited, and a few lookups. Declared by hand, as no crates are used; the layouts are
//! the Windows SDK's.

#![allow(non_snake_case, clippy::upper_case_acronyms)]

use std::ffi::{c_void, OsStr};
use std::os::windows::ffi::OsStrExt;

pub type HANDLE = isize;
pub type BOOL = i32;

pub const INVALID_HANDLE_VALUE: HANDLE = -1;

#[repr(C)]
pub struct Overlapped {
    pub internal: usize,
    pub internal_high: usize,
    pub offset: u32,
    pub offset_high: u32,
    pub event: HANDLE,
}

#[repr(C)]
pub struct SecurityAttributes {
    pub length: u32,
    pub descriptor: *mut c_void,
    pub inherit: BOOL,
}

#[repr(C)]
pub struct StartupInfoW {
    pub cb: u32,
    pub reserved: *mut u16,
    pub desktop: *mut u16,
    pub title: *mut u16,
    pub x: u32,
    pub y: u32,
    pub x_size: u32,
    pub y_size: u32,
    pub x_chars: u32,
    pub y_chars: u32,
    pub fill: u32,
    pub flags: u32,
    pub show_window: u16,
    pub reserved2_n: u16,
    pub reserved2: *mut u8,
    pub std_input: HANDLE,
    pub std_output: HANDLE,
    pub std_error: HANDLE,
}

#[repr(C)]
pub struct StartupInfoExW {
    pub si: StartupInfoW,
    pub attrs: *mut c_void,
}

#[repr(C)]
pub struct ProcessInformation {
    pub process: HANDLE,
    pub thread: HANDLE,
    pub pid: u32,
    pub tid: u32,
}

#[repr(C)]
#[derive(Default)]
pub struct JobBasicLimits {
    pub per_process_user_time: i64,
    pub per_job_user_time: i64,
    pub flags: u32,
    pub min_working_set: usize,
    pub max_working_set: usize,
    pub active_process_limit: u32,
    pub affinity: usize,
    pub priority_class: u32,
    pub scheduling_class: u32,
}

#[repr(C)]
#[derive(Default)]
pub struct JobExtendedLimits {
    pub basic: JobBasicLimits,
    pub io: [u64; 6],
    pub process_memory_limit: usize,
    pub job_memory_limit: usize,
    pub peak_process_memory: usize,
    pub peak_job_memory: usize,
}

/// INPUT_RECORD, with its union read as a KEY_EVENT_RECORD (the only kind barb looks inside).
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct InputRecord {
    pub event_type: u16,
    pub _pad: u16,
    pub key_down: BOOL,
    pub repeat: u16,
    pub vk: u16,
    pub scan: u16,
    pub ch: u16,
    pub ctrl_state: u32,
}

#[repr(C)]
#[derive(Default)]
pub struct ScreenBufferInfo {
    pub size_x: i16,
    pub size_y: i16,
    pub cursor_x: i16,
    pub cursor_y: i16,
    pub attributes: u16,
    pub window: [i16; 4],
    pub max_x: i16,
    pub max_y: i16,
}

#[repr(C)]
pub struct SystemInfo {
    pub arch: u16,
    pub reserved: u16,
    pub page_size: u32,
    pub min_addr: *mut c_void,
    pub max_addr: *mut c_void,
    pub active_mask: usize,
    pub processors: u32,
    pub processor_type: u32,
    pub granularity: u32,
    pub level: u16,
    pub revision: u16,
}

#[repr(C)]
pub struct OsVersionInfoW {
    pub size: u32,
    pub major: u32,
    pub minor: u32,
    pub build: u32,
    pub platform: u32,
    pub csd: [u16; 128],
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct SystemTime {
    pub year: u16,
    pub month: u16,
    pub day_of_week: u16,
    pub day: u16,
    pub hour: u16,
    pub minute: u16,
    pub second: u16,
    pub ms: u16,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct FileTime {
    pub low: u32,
    pub high: u32,
}

pub const STD_INPUT_HANDLE: u32 = -10i32 as u32;
pub const STD_OUTPUT_HANDLE: u32 = -11i32 as u32;
pub const STD_ERROR_HANDLE: u32 = -12i32 as u32;

pub const ENABLE_PROCESSED_INPUT: u32 = 0x1;
pub const ENABLE_LINE_INPUT: u32 = 0x2;
pub const ENABLE_ECHO_INPUT: u32 = 0x4;
pub const ENABLE_WINDOW_INPUT: u32 = 0x8;
pub const ENABLE_PROCESSED_OUTPUT: u32 = 0x1;
pub const ENABLE_VIRTUAL_TERMINAL_PROCESSING: u32 = 0x4;

pub const KEY_EVENT: u16 = 1;
pub const WINDOW_BUFFER_SIZE_EVENT: u16 = 4;
pub const LEFT_CTRL_PRESSED: u32 = 0x8;
pub const RIGHT_CTRL_PRESSED: u32 = 0x4;
pub const VK_MENU: u16 = 0x12;
pub const VK_END: u16 = 0x23;
pub const VK_HOME: u16 = 0x24;
pub const VK_LEFT: u16 = 0x25;
pub const VK_UP: u16 = 0x26;
pub const VK_RIGHT: u16 = 0x27;
pub const VK_DOWN: u16 = 0x28;
pub const VK_DELETE: u16 = 0x2E;

pub const WAIT_OBJECT_0: u32 = 0;
pub const WAIT_TIMEOUT: u32 = 0x102;
pub const INFINITE: u32 = 0xFFFF_FFFF;
pub const MAXIMUM_WAIT_OBJECTS: usize = 64;

pub const CTRL_C_EVENT: u32 = 0;
pub const CTRL_BREAK_EVENT: u32 = 1;

pub const PIPE_ACCESS_INBOUND: u32 = 0x1;
pub const FILE_FLAG_OVERLAPPED: u32 = 0x4000_0000;
pub const FILE_FLAG_FIRST_PIPE_INSTANCE: u32 = 0x0008_0000;
pub const PIPE_TYPE_BYTE: u32 = 0;
pub const PIPE_WAIT: u32 = 0;
pub const PIPE_REJECT_REMOTE_CLIENTS: u32 = 0x8;
pub const GENERIC_READ: u32 = 0x8000_0000;
pub const GENERIC_WRITE: u32 = 0x4000_0000;
pub const FILE_READ_ATTRIBUTES: u32 = 0x80;
pub const FILE_SHARE_ALL: u32 = 0x1 | 0x2 | 0x4;
pub const FILE_SHARE_READ_WRITE: u32 = 0x1 | 0x2;
pub const OPEN_EXISTING: u32 = 3;
pub const CREATE_ALWAYS: u32 = 2;
pub const FILE_ATTRIBUTE_NORMAL: u32 = 0x80;
pub const FILE_FLAG_SEQUENTIAL_SCAN: u32 = 0x0800_0000;
pub const FILE_FLAG_BACKUP_SEMANTICS: u32 = 0x0200_0000;
pub const FILE_TYPE_DISK: u32 = 1;

pub const ERROR_OUTOFMEMORY: u32 = 14;
pub const ERROR_WRITE_FAULT: u32 = 29;
pub const ERROR_IO_INCOMPLETE: u32 = 996;
pub const ERROR_IO_PENDING: u32 = 997;

pub const PROC_THREAD_ATTRIBUTE_HANDLE_LIST: usize = 0x20002;
pub const CREATE_SUSPENDED: u32 = 0x4;
pub const CREATE_UNICODE_ENVIRONMENT: u32 = 0x400;
pub const EXTENDED_STARTUPINFO_PRESENT: u32 = 0x0008_0000;
pub const CREATE_NO_WINDOW: u32 = 0x0800_0000;
pub const STARTF_USESTDHANDLES: u32 = 0x100;
pub const HANDLE_FLAG_INHERIT: u32 = 0x1;
pub const JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: u32 = 0x2000;
pub const JOB_OBJECT_EXTENDED_LIMIT_INFORMATION: i32 = 9;

pub const FORMAT_MESSAGE_FROM_SYSTEM: u32 = 0x1000;
pub const FORMAT_MESSAGE_IGNORE_INSERTS: u32 = 0x200;
pub const LANG_NEUTRAL_DEFAULT: u32 = 0x400;

pub const REPLACEFILE_IGNORE_MERGE_ERRORS: u32 = 0x2;
pub const REPLACEFILE_IGNORE_ACL_ERRORS: u32 = 0x4;

pub const HKEY_LOCAL_MACHINE: HANDLE = 0x8000_0002u32 as i32 as isize;
pub const HKEY_CURRENT_USER: HANDLE = 0x8000_0001u32 as i32 as isize;
pub const RRF_RT_REG_SZ: u32 = 0x2;

pub const PROCESSOR_ARCHITECTURE_INTEL: u16 = 0;
pub const PROCESSOR_ARCHITECTURE_AMD64: u16 = 9;
pub const PROCESSOR_ARCHITECTURE_ARM64: u16 = 12;

pub type CtrlHandler = unsafe extern "system" fn(u32) -> BOOL;

#[link(name = "kernel32")]
extern "system" {
    pub fn GetStdHandle(which: u32) -> HANDLE;
    pub fn GetConsoleMode(h: HANDLE, mode: *mut u32) -> BOOL;
    pub fn SetConsoleMode(h: HANDLE, mode: u32) -> BOOL;
    pub fn WriteConsoleW(h: HANDLE, p: *const u16, n: u32, wrote: *mut u32, reserved: *mut c_void) -> BOOL;
    pub fn ReadConsoleW(h: HANDLE, p: *mut u16, n: u32, got: *mut u32, control: *mut c_void) -> BOOL;
    pub fn GetConsoleScreenBufferInfo(h: HANDLE, info: *mut ScreenBufferInfo) -> BOOL;
    pub fn GetNumberOfConsoleInputEvents(h: HANDLE, n: *mut u32) -> BOOL;
    pub fn ReadConsoleInputW(h: HANDLE, r: *mut InputRecord, n: u32, got: *mut u32) -> BOOL;
    pub fn PeekConsoleInputW(h: HANDLE, r: *mut InputRecord, n: u32, got: *mut u32) -> BOOL;
    pub fn SetConsoleCtrlHandler(handler: Option<CtrlHandler>, add: BOOL) -> BOOL;

    pub fn ReadFile(h: HANDLE, p: *mut u8, n: u32, got: *mut u32, ov: *mut Overlapped) -> BOOL;
    pub fn WriteFile(h: HANDLE, p: *const u8, n: u32, wrote: *mut u32, ov: *mut Overlapped) -> BOOL;
    pub fn CloseHandle(h: HANDLE) -> BOOL;
    pub fn GetLastError() -> u32;

    pub fn CreateEventW(sa: *mut SecurityAttributes, manual: BOOL, initial: BOOL, name: *const u16) -> HANDLE;
    pub fn SetEvent(h: HANDLE) -> BOOL;
    pub fn ResetEvent(h: HANDLE) -> BOOL;
    pub fn WaitForSingleObject(h: HANDLE, ms: u32) -> u32;
    pub fn WaitForMultipleObjects(n: u32, h: *const HANDLE, all: BOOL, ms: u32) -> u32;
    pub fn GetTickCount64() -> u64;

    pub fn CreateNamedPipeW(
        name: *const u16,
        open_mode: u32,
        pipe_mode: u32,
        instances: u32,
        out_size: u32,
        in_size: u32,
        timeout: u32,
        sa: *mut SecurityAttributes,
    ) -> HANDLE;
    pub fn CreateFileW(
        name: *const u16,
        access: u32,
        share: u32,
        sa: *mut SecurityAttributes,
        disposition: u32,
        flags: u32,
        template: HANDLE,
    ) -> HANDLE;
    pub fn GetOverlappedResult(h: HANDLE, ov: *mut Overlapped, got: *mut u32, wait: BOOL) -> BOOL;
    pub fn CancelIoEx(h: HANDLE, ov: *mut Overlapped) -> BOOL;
    pub fn CreatePipe(r: *mut HANDLE, w: *mut HANDLE, sa: *mut SecurityAttributes, size: u32) -> BOOL;
    pub fn SetHandleInformation(h: HANDLE, mask: u32, flags: u32) -> BOOL;
    pub fn GetFileType(h: HANDLE) -> u32;

    pub fn InitializeProcThreadAttributeList(list: *mut c_void, count: u32, flags: u32, size: *mut usize) -> BOOL;
    pub fn UpdateProcThreadAttribute(
        list: *mut c_void,
        flags: u32,
        attr: usize,
        value: *mut c_void,
        size: usize,
        prev: *mut c_void,
        ret: *mut usize,
    ) -> BOOL;
    pub fn DeleteProcThreadAttributeList(list: *mut c_void);
    pub fn CreateProcessW(
        app: *const u16,
        cmdline: *mut u16,
        psa: *mut SecurityAttributes,
        tsa: *mut SecurityAttributes,
        inherit: BOOL,
        flags: u32,
        env: *mut c_void,
        cwd: *const u16,
        si: *mut StartupInfoW,
        pi: *mut ProcessInformation,
    ) -> BOOL;
    pub fn ResumeThread(h: HANDLE) -> u32;
    pub fn GetExitCodeProcess(h: HANDLE, code: *mut u32) -> BOOL;
    pub fn TerminateProcess(h: HANDLE, code: u32) -> BOOL;
    pub fn CreateJobObjectW(sa: *mut SecurityAttributes, name: *const u16) -> HANDLE;
    pub fn SetInformationJobObject(job: HANDLE, class: i32, info: *mut c_void, size: u32) -> BOOL;
    pub fn AssignProcessToJobObject(job: HANDLE, process: HANDLE) -> BOOL;
    pub fn TerminateJobObject(job: HANDLE, code: u32) -> BOOL;
    pub fn GetCurrentProcessId() -> u32;

    pub fn GetTempPathW(n: u32, buf: *mut u16) -> u32;
    pub fn GetTempFileNameW(dir: *const u16, prefix: *const u16, unique: u32, out: *mut u16) -> u32;
    pub fn GetShortPathNameW(long: *const u16, short: *mut u16, n: u32) -> u32;
    pub fn GetFileAttributesW(name: *const u16) -> u32;
    pub fn GetSystemDirectoryW(buf: *mut u16, n: u32) -> u32;
    pub fn ReplaceFileW(
        replaced: *const u16,
        replacement: *const u16,
        backup: *const u16,
        flags: u32,
        exclude: *mut c_void,
        reserved: *mut c_void,
    ) -> BOOL;
    pub fn FormatMessageW(
        flags: u32,
        source: *const c_void,
        id: u32,
        lang: u32,
        buf: *mut u16,
        n: u32,
        args: *mut c_void,
    ) -> u32;

    pub fn GetNativeSystemInfo(si: *mut SystemInfo);
    pub fn GetModuleHandleW(name: *const u16) -> HANDLE;
    pub fn GetProcAddress(module: HANDLE, name: *const u8) -> *const c_void;
    pub fn GetSystemTimeAsFileTime(ft: *mut FileTime);
    pub fn FileTimeToSystemTime(ft: *const FileTime, st: *mut SystemTime) -> BOOL;
    pub fn SystemTimeToTzSpecificLocalTime(tz: *const c_void, utc: *const SystemTime, local: *mut SystemTime) -> BOOL;
}

#[link(name = "advapi32")]
extern "system" {
    pub fn RegGetValueW(
        key: HANDLE,
        sub: *const u16,
        value: *const u16,
        flags: u32,
        kind: *mut u32,
        data: *mut c_void,
        size: *mut u32,
    ) -> i32;
}

// ---- helpers ----

/// A string as Windows wants it: UTF-16 with a NUL at the end.
pub fn wide(s: impl AsRef<OsStr>) -> Vec<u16> {
    s.as_ref().encode_wide().chain(std::iter::once(0)).collect()
}

/// UTF-8, as the agent speaks, to UTF-16 (invalid bytes become U+FFFD).
pub fn utf16(b: &[u8]) -> Vec<u16> {
    String::from_utf8_lossy(b).encode_utf16().collect()
}

pub fn utf8(w: &[u16]) -> Vec<u8> {
    String::from_utf16_lossy(w).into_bytes()
}

/// Without the NUL at the end, if there is one.
pub fn unz(w: &[u16]) -> &[u16] {
    match w.iter().position(|&c| c == 0) {
        Some(i) => &w[..i],
        None => w,
    }
}

pub fn last_error() -> u32 {
    // SAFETY: no arguments; reads the thread's last-error value.
    unsafe { GetLastError() }
}

pub fn close(h: HANDLE) {
    if h != 0 && h != INVALID_HANDLE_VALUE {
        // SAFETY: h is a handle this program opened and owns; it is closed once.
        unsafe { CloseHandle(h) };
    }
}

/// Milliseconds since boot: the clock everything is timed by.
pub fn now() -> i64 {
    // SAFETY: no arguments.
    unsafe { GetTickCount64() as i64 }
}

/// What Windows says about an error, without the trailing period and line break.
pub fn why(err: u32) -> String {
    let mut w = [0u16; 400];
    // SAFETY: w is a writable buffer of the size given; no inserts are read.
    let mut k = unsafe {
        FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            std::ptr::null(),
            err,
            LANG_NEUTRAL_DEFAULT,
            w.as_mut_ptr(),
            w.len() as u32,
            std::ptr::null_mut(),
        )
    } as usize;
    while k > 0 && matches!(w[k - 1], 0x0A | 0x0D | 0x20 | 0x2E) {
        k -= 1;
    }
    if k == 0 {
        return format!("Windows error {}", err);
    }
    String::from_utf16_lossy(&w[..k])
}

/// The variable's value, or None if it is not set.
pub fn env(name: &str) -> Option<Vec<u16>> {
    std::env::var_os(name).map(|v| v.encode_wide().collect())
}

pub fn is_file(p: &[u16]) -> bool {
    let z: Vec<u16> = unz(p).iter().copied().chain(std::iter::once(0)).collect();
    // SAFETY: z is NUL-terminated.
    let a = unsafe { GetFileAttributesW(z.as_ptr()) };
    a != 0xFFFF_FFFF && a & 0x10 == 0
}

/// `a` and `b` with one backslash between them.
pub fn join(a: &[u16], b: &str) -> Vec<u16> {
    let mut r = unz(a).to_vec();
    if let Some(&c) = r.last() {
        if c != b'\\' as u16 && c != b'/' as u16 {
            r.push(b'\\' as u16);
        }
    }
    r.extend(b.encode_utf16());
    r
}

/// The local time `delta_ms` from now: (year, month, day, hour, minute).
pub fn local_time(delta_ms: i64) -> Option<(i32, i32, i32, i32, i32)> {
    let mut ft = FileTime::default();
    // SAFETY: ft is a valid out-pointer.
    unsafe { GetSystemTimeAsFileTime(&mut ft) };
    let t = (((ft.high as u64) << 32) | ft.low as u64) as i64 + delta_ms * 10_000;
    let ft = FileTime { low: t as u32, high: (t as u64 >> 32) as u32 };
    let (mut utc, mut loc) = (SystemTime::default(), SystemTime::default());
    // SAFETY: valid pointers to initialised structs; a null zone means the current one.
    let ok = unsafe { FileTimeToSystemTime(&ft, &mut utc) != 0 && SystemTimeToTzSpecificLocalTime(std::ptr::null(), &utc, &mut loc) != 0 };
    ok.then(|| (loc.year as i32, loc.month as i32, loc.day as i32, loc.hour as i32, loc.minute as i32))
}
