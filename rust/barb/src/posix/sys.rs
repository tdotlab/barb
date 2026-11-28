//! The few C library calls std does not wrap, declared by hand (Barb has no dependencies), for
//! Linux and macOS. Where the two lay a structure out differently, both layouts are here.
#![allow(non_camel_case_types, dead_code)]

use std::os::raw::{c_char, c_int, c_long, c_ulong, c_void};

#[cfg(not(any(target_os = "linux", target_os = "android", target_vendor = "apple")))]
compile_error!("barb's POSIX side knows the C library layouts of Linux and macOS only");

pub type time_t = i64;

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct PollFd {
    pub fd: c_int,
    pub events: i16,
    pub revents: i16,
}

pub const POLLIN: i16 = 0x1;
pub const POLLERR: i16 = 0x8;
pub const POLLHUP: i16 = 0x10;
pub const POLLNVAL: i16 = 0x20;

pub const FD_SETSIZE: usize = 1024;

#[cfg(any(target_os = "linux", target_os = "android"))]
type FdWord = c_ulong;
#[cfg(target_vendor = "apple")]
type FdWord = i32;
const FD_BITS: usize = 8 * std::mem::size_of::<FdWord>();

#[repr(C)]
pub struct FdSet {
    bits: [FdWord; FD_SETSIZE / FD_BITS],
}

impl FdSet {
    pub fn new() -> FdSet {
        FdSet { bits: [0; FD_SETSIZE / FD_BITS] }
    }
    pub fn set(&mut self, fd: c_int) {
        let fd = fd as usize;
        self.bits[fd / FD_BITS] |= (1 as FdWord) << (fd % FD_BITS);
    }
    pub fn has(&self, fd: c_int) -> bool {
        let fd = fd as usize;
        self.bits[fd / FD_BITS] & ((1 as FdWord) << (fd % FD_BITS)) != 0
    }
}

#[cfg(any(target_os = "linux", target_os = "android"))]
#[repr(C)]
pub struct Timeval {
    pub sec: c_long,
    pub usec: c_long,
}
#[cfg(target_vendor = "apple")]
#[repr(C)]
pub struct Timeval {
    pub sec: i64,
    pub usec: i32,
}

// ---- the terminal ----

#[cfg(any(target_os = "linux", target_os = "android"))]
mod tty {
    pub type tcflag_t = u32;
    pub const NCCS: usize = 32;
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct Termios {
        pub iflag: tcflag_t,
        pub oflag: tcflag_t,
        pub cflag: tcflag_t,
        pub lflag: tcflag_t,
        pub line: u8,
        pub cc: [u8; NCCS],
        pub ispeed: u32,
        pub ospeed: u32,
    }
    pub const ICANON: tcflag_t = 0o2;
    pub const ECHO: tcflag_t = 0o10;
    pub const IEXTEN: tcflag_t = 0o100000;
    pub const INLCR: tcflag_t = 0o100;
    pub const IGNCR: tcflag_t = 0o200;
    pub const ICRNL: tcflag_t = 0o400;
    pub const IXON: tcflag_t = 0o2000;
    pub const VTIME: usize = 5;
    pub const VMIN: usize = 6;
    pub const VQUIT: usize = 1;
    pub const VDISABLE: u8 = 0;
    pub const TCSANOW: i32 = 0;
    pub const TCIFLUSH: i32 = 0;
    pub const TIOCGWINSZ: u64 = 0x5413;
}

#[cfg(target_vendor = "apple")]
mod tty {
    pub type tcflag_t = u64;
    pub const NCCS: usize = 20;
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct Termios {
        pub iflag: tcflag_t,
        pub oflag: tcflag_t,
        pub cflag: tcflag_t,
        pub lflag: tcflag_t,
        pub cc: [u8; NCCS],
        pub ispeed: u64,
        pub ospeed: u64,
    }
    pub const ECHO: tcflag_t = 0x8;
    pub const ICANON: tcflag_t = 0x100;
    pub const IEXTEN: tcflag_t = 0x400;
    pub const INLCR: tcflag_t = 0x40;
    pub const IGNCR: tcflag_t = 0x80;
    pub const ICRNL: tcflag_t = 0x100;
    pub const IXON: tcflag_t = 0x200;
    pub const VQUIT: usize = 9;
    pub const VMIN: usize = 16;
    pub const VTIME: usize = 17;
    pub const VDISABLE: u8 = 0xff;
    pub const TCSANOW: i32 = 0;
    pub const TCIFLUSH: i32 = 1;
    pub const TIOCGWINSZ: u64 = 0x40087468;
}

pub use tty::*;

#[repr(C)]
#[derive(Default)]
pub struct Winsize {
    pub row: u16,
    pub col: u16,
    pub xpixel: u16,
    pub ypixel: u16,
}

// ---- signals ----

pub const SIGINT: c_int = 2;
pub const SIGKILL: c_int = 9;
pub const SIGTERM: c_int = 15;
#[cfg(any(target_os = "linux", target_os = "android"))]
mod sigs {
    pub const SIGCONT: i32 = 18;
    pub const SIGTSTP: i32 = 20;
    pub const SIGWINCH: i32 = 28;
}
#[cfg(target_vendor = "apple")]
mod sigs {
    pub const SIGTSTP: i32 = 18;
    pub const SIGCONT: i32 = 19;
    pub const SIGWINCH: i32 = 28;
}
pub use sigs::*;

pub const SIG_DFL: usize = 0;

// ---- the rest ----

#[repr(C)]
pub struct Tm {
    pub sec: c_int,
    pub min: c_int,
    pub hour: c_int,
    pub mday: c_int,
    pub mon: c_int,
    pub year: c_int,
    pub wday: c_int,
    pub yday: c_int,
    pub isdst: c_int,
    pub gmtoff: c_long,
    pub zone: *const c_char,
}

#[cfg(any(target_os = "linux", target_os = "android"))]
pub const UTS_LEN: usize = 65;
#[cfg(any(target_os = "linux", target_os = "android"))]
pub const UTS_FIELDS: usize = 6;
#[cfg(target_vendor = "apple")]
pub const UTS_LEN: usize = 256;
#[cfg(target_vendor = "apple")]
pub const UTS_FIELDS: usize = 5;

pub const F_GETFL: c_int = 3;
pub const F_SETFL: c_int = 4;
#[cfg(any(target_os = "linux", target_os = "android"))]
pub const O_NONBLOCK: c_int = 0o4000;
#[cfg(target_vendor = "apple")]
pub const O_NONBLOCK: c_int = 0x4;

pub const EBADF: i32 = 9;

extern "C" {
    #[cfg_attr(all(target_vendor = "apple", target_arch = "x86_64"), link_name = "select$1050")]
    fn select(n: c_int, r: *mut FdSet, w: *mut FdSet, e: *mut FdSet, tv: *mut Timeval) -> c_int;
    fn poll(fds: *mut PollFd, n: c_ulong, timeout: c_int) -> c_int;
    fn fcntl(fd: c_int, cmd: c_int, ...) -> c_int;
    fn tcgetattr(fd: c_int, t: *mut Termios) -> c_int;
    fn tcsetattr(fd: c_int, how: c_int, t: *const Termios) -> c_int;
    fn tcflush(fd: c_int, how: c_int) -> c_int;
    fn ioctl(fd: c_int, req: c_ulong, ...) -> c_int;
    fn signal(sig: c_int, handler: usize) -> usize;
    fn raise(sig: c_int) -> c_int;
    fn kill(pid: c_int, sig: c_int) -> c_int;
    fn uname(buf: *mut u8) -> c_int;
    fn localtime_r(t: *const time_t, tm: *mut Tm) -> *mut Tm;
    fn mkstemp(template: *mut c_char) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn write(fd: c_int, p: *const c_void, n: usize) -> isize;
    #[cfg(any(target_os = "linux", target_os = "android"))]
    #[cfg_attr(target_os = "android", link_name = "__errno")]
    fn __errno_location() -> *mut c_int;
    #[cfg(target_vendor = "apple")]
    fn __error() -> *mut c_int;
}

fn errno_ptr() -> *mut c_int {
    // SAFETY: both return the calling thread's errno, which is always valid.
    #[cfg(any(target_os = "linux", target_os = "android"))]
    unsafe {
        __errno_location()
    }
    #[cfg(target_vendor = "apple")]
    unsafe {
        __error()
    }
}

pub fn errno() -> c_int {
    // SAFETY: see errno_ptr.
    unsafe { *errno_ptr() }
}

pub fn set_errno(e: c_int) {
    // SAFETY: see errno_ptr.
    unsafe { *errno_ptr() = e }
}

/// Waits like poll(2) for input on the descriptors in fds, but through select(2): Apple's poll()
/// does not support terminals. Descriptors that are not readable get revents 0; end of file counts
/// as readable (the read then returns 0). A negative fd is ignored. timeout_ms < 0 waits for ever.
/// A descriptor select cannot take (one past FD_SETSIZE, or a bad one) is left to poll, which
/// says which.
pub fn wait(fds: &mut [PollFd], timeout_ms: i32) -> i32 {
    let mut rd = FdSet::new();
    let mut maxfd = -1;
    for f in fds.iter_mut() {
        f.revents = 0;
        if f.fd < 0 {
            continue;
        }
        if f.fd as usize >= FD_SETSIZE {
            return poll_all(fds, timeout_ms);
        }
        rd.set(f.fd);
        maxfd = maxfd.max(f.fd);
    }
    let mut tv = Timeval { sec: (timeout_ms / 1000) as _, usec: ((timeout_ms % 1000) * 1000) as _ };
    let tp: *mut Timeval = if timeout_ms >= 0 { &mut tv } else { std::ptr::null_mut() };
    // SAFETY: rd and tv outlive the call; the other sets are null, which select allows.
    let r = unsafe { select(maxfd + 1, &mut rd, std::ptr::null_mut(), std::ptr::null_mut(), tp) };
    if r < 0 && errno() == EBADF {
        return poll_all(fds, timeout_ms);
    }
    if r <= 0 {
        return r;
    }
    let mut ready = 0;
    for f in fds.iter_mut() {
        if f.fd >= 0 && rd.has(f.fd) {
            f.revents = POLLIN;
            ready += 1;
        }
    }
    ready
}

fn poll_all(fds: &mut [PollFd], timeout_ms: i32) -> i32 {
    // SAFETY: fds is a valid array of fds.len() pollfd structures.
    unsafe { poll(fds.as_mut_ptr(), fds.len() as c_ulong, timeout_ms) }
}

pub fn set_nonblock(fd: c_int) {
    // SAFETY: fcntl on any descriptor is harmless; a bad one returns an error.
    unsafe {
        let f = fcntl(fd, F_GETFL);
        if f >= 0 {
            fcntl(fd, F_SETFL, f | O_NONBLOCK);
        }
    }
}

pub fn get_term(fd: c_int) -> Option<Termios> {
    let mut t = std::mem::MaybeUninit::<Termios>::uninit();
    // SAFETY: tcgetattr fills the whole structure when it succeeds.
    unsafe {
        if tcgetattr(fd, t.as_mut_ptr()) == 0 {
            Some(t.assume_init())
        } else {
            None
        }
    }
}

pub fn set_term(fd: c_int, t: &Termios) -> bool {
    // SAFETY: t is a valid termios structure.
    unsafe { tcsetattr(fd, TCSANOW, t) == 0 }
}

pub fn flush_input(fd: c_int) {
    // SAFETY: harmless on any descriptor.
    unsafe { tcflush(fd, TCIFLUSH) };
}

/// The columns of the terminal on fd, or None.
pub fn columns(fd: c_int) -> Option<usize> {
    let mut w = Winsize::default();
    // SAFETY: TIOCGWINSZ writes one winsize.
    let r = unsafe { ioctl(fd, TIOCGWINSZ as c_ulong, &mut w as *mut Winsize) };
    if r == 0 && w.col > 0 {
        Some(w.col as usize)
    } else {
        None
    }
}

/// Catch `sig` with `handler`, or give it its default action (`handler` 0).
pub fn on_signal(sig: c_int, handler: usize) {
    // SAFETY: the handler is an extern "C" fn(c_int) or SIG_DFL. The C library's signal() keeps
    // the handler installed (BSD semantics, on glibc, musl and macOS alike).
    unsafe { signal(sig, handler) };
}

pub fn raise_signal(sig: c_int) {
    // SAFETY: raising a signal for which a disposition is set.
    unsafe { raise(sig) };
}

/// A signal to a process group.
pub fn kill_group(pid: i32, sig: c_int) {
    if pid > 0 {
        // SAFETY: kill with a negative pid signals the group; at worst it fails.
        unsafe { kill(-pid, sig) };
    }
}

/// "Linux x86_64", "Darwin arm64": the system's name and its machine.
pub fn platform() -> String {
    let mut buf = [0u8; UTS_LEN * UTS_FIELDS];
    // SAFETY: buf is as big as struct utsname on this system.
    if unsafe { uname(buf.as_mut_ptr()) } != 0 {
        return "unknown".to_string();
    }
    let field = |i: usize| {
        let f = &buf[i * UTS_LEN..(i + 1) * UTS_LEN];
        let n = f.iter().position(|&c| c == 0).unwrap_or(f.len());
        String::from_utf8_lossy(&f[..n]).into_owned()
    };
    format!("{} {}", field(0), field(4))
}

/// The local time `t` (seconds since 1970): year, month (1-12), day, hour, minute.
pub fn local(t: i64) -> Option<(i32, i32, i32, i32, i32)> {
    let mut tm = std::mem::MaybeUninit::<Tm>::uninit();
    // SAFETY: localtime_r fills tm when it returns non-null.
    unsafe {
        if localtime_r(&t, tm.as_mut_ptr()).is_null() {
            return None;
        }
        let tm = tm.assume_init();
        Some((tm.year + 1900, tm.mon + 1, tm.mday, tm.hour, tm.min))
    }
}

/// A new private file from a template ending in XXXXXX (changed to the name made), or None.
pub fn make_temp(template: &mut Vec<u8>) -> Option<std::fs::File> {
    use std::os::unix::io::FromRawFd;
    template.push(0);
    // SAFETY: the template is NUL-terminated and writable.
    let fd = unsafe { mkstemp(template.as_mut_ptr() as *mut c_char) };
    template.pop();
    if fd < 0 {
        return None;
    }
    // SAFETY: mkstemp returned a new descriptor that nothing else owns.
    Some(unsafe { std::fs::File::from_raw_fd(fd) })
}

/// Close a descriptor, saying whether it went well (std's File ignores close's errors, which
/// on some filesystems are where a write fails).
pub fn close_fd(fd: c_int) -> std::io::Result<()> {
    // SAFETY: the caller owns fd and gives it up.
    if unsafe { close(fd) } == 0 {
        Ok(())
    } else {
        Err(std::io::Error::last_os_error())
    }
}

/// write(2) as a signal handler may call it.
pub fn write_raw(fd: c_int, b: &[u8]) -> isize {
    // SAFETY: b is valid for b.len() bytes.
    unsafe { write(fd, b.as_ptr() as *const c_void, b.len()) }
}

#[cfg(test)]
mod tests {
    // The waiting the loop does, as tests/test_wait.c checked it in C.
    use super::*;
    use std::io::Write;
    use std::os::unix::io::AsRawFd;
    use std::time::Instant;

    fn pipe() -> (std::io::PipeReader, std::io::PipeWriter) {
        std::io::pipe().unwrap()
    }

    fn pfd(fd: c_int) -> PollFd {
        PollFd { fd, events: POLLIN, revents: 0x7f }
    }

    #[test]
    fn nothing_ready_times_out() {
        let (r, _w) = pipe();
        let mut f = [pfd(r.as_raw_fd())];
        assert_eq!(wait(&mut f, 0), 0);
        assert_eq!(f[0].revents, 0, "revents cleared");
        let t = Instant::now();
        assert_eq!(wait(&mut f, 60), 0);
        let ms = t.elapsed().as_millis();
        assert!((50..1000).contains(&ms), "60ms wait took {}ms", ms);
        let t = Instant::now();
        assert_eq!(wait(&mut f, 1150), 0, "a timeout over a second");
        let ms = t.elapsed().as_millis();
        assert!((1100..3000).contains(&ms), "1150ms wait took {}ms", ms);
    }

    #[test]
    fn ready_returns_at_once() {
        let (r1, _w1) = pipe();
        let (r2, mut w2) = pipe();
        let (r3, mut w3) = pipe();
        w2.write_all(b"x").unwrap();
        w3.write_all(b"y").unwrap();
        let mut f = [pfd(r1.as_raw_fd()), pfd(-1), pfd(r2.as_raw_fd()), pfd(r3.as_raw_fd())];
        let t = Instant::now();
        assert_eq!(wait(&mut f, 5000), 2, "two of them");
        assert!(t.elapsed().as_millis() < 1000);
        assert_eq!([f[0].revents, f[1].revents, f[2].revents, f[3].revents], [0, 0, POLLIN, POLLIN], "the negative fd skipped");
    }

    #[test]
    fn end_of_file_is_readable() {
        let (r, w) = pipe();
        drop(w);
        let mut f = [pfd(r.as_raw_fd())];
        assert_eq!(wait(&mut f, 1000), 1);
        assert_eq!(f[0].revents, POLLIN);
    }

    #[test]
    fn bad_descriptors_go_to_poll() {
        let mut f = [pfd(900)];
        let r = wait(&mut f, 0);
        assert!(r == 0 || (r == 1 && f[0].revents & POLLNVAL != 0), "a closed fd: {} {}", r, f[0].revents);
        // Past FD_SETSIZE: select cannot take it, poll can.
        let (r1, mut w1) = pipe();
        w1.write_all(b"z").unwrap();
        let big = 1100;
        // SAFETY: dup2 onto a free high descriptor, closed again below.
        extern "C" {
            fn dup2(a: c_int, b: c_int) -> c_int;
        }
        let got = unsafe { dup2(r1.as_raw_fd(), big) };
        if got != big {
            return; // the descriptor limit is lower here: nothing to test
        }
        let mut f = [pfd(big)];
        assert_eq!(wait(&mut f, 1000), 1);
        assert_eq!(f[0].revents & POLLIN, POLLIN);
        close_fd(big).unwrap();
    }
}
