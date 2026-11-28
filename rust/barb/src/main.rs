//! barb, the program: the command line and the platform layer that carries out what the agent
//! (barb-core) decides, shown through barb-ui. Only this crate touches the system.

mod common;
#[cfg(unix)]
mod posix;
#[cfg(windows)]
mod win32;

fn main() {
    #[cfg(unix)]
    let code = posix::main();
    #[cfg(windows)]
    let code = win32::main();
    std::process::exit(code);
}
