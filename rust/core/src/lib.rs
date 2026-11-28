//! Barb's core: the agent as a state machine that turns events into actions. It makes no system
//! calls and needs only an allocator, so any platform (POSIX, Windows, a kernel with no libc)
//! can drive it.
#![no_std]
#![forbid(unsafe_code)]
extern crate alloc;

#[macro_use]
pub mod text;
pub mod api;
pub mod json;
pub mod agent;
pub mod tools;
