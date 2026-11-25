//! Barb's terminal interface, apart from the system: the prompt's line editor, the key decoder,
//! the markdown renderer and the screen. Each takes input and returns the bytes to write, so the
//! tests can play them on an emulated terminal and every platform can share them.
#![no_std]
#![forbid(unsafe_code)]
extern crate alloc;

pub mod edit;
pub mod keys;
pub mod md;
pub mod screen;
