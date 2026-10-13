//! By hand: plays a stream for a while and says how much sound came of it.
//!   cargo run --release --example decode -- run <program> <argument>...
//!   cargo run --release --example decode -- file <path>
//!   cargo run --release --example decode -- url <address> [seconds]
use std::ffi::{c_char, CStr, CString};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

static FRAMES: AtomicU64 = AtomicU64::new(0);
static RATE: AtomicU64 = AtomicU64::new(0);
static DONE: AtomicBool = AtomicBool::new(false);

extern "C" fn sound(_samples: *const f32, frames: u32, rate: u32) {
    FRAMES.fetch_add(u64::from(frames), Ordering::Relaxed);
    RATE.store(u64::from(rate), Ordering::Relaxed);
}
extern "C" fn ended(stream: u64, how: *const c_char) {
    println!("stream {stream}: {}", unsafe { CStr::from_ptr(how) }.to_string_lossy());
    DONE.store(true, Ordering::Relaxed);
}
extern "C" fn titled(_stream: u64, title: *const c_char) {
    println!("playing: {}", unsafe { CStr::from_ptr(title) }.to_string_lossy());
}

fn main() {
    let mut arguments = std::env::args().skip(1);
    let how = arguments.next().expect("run, file or url");
    let first = CString::new(arguments.next().expect("what to play")).unwrap();
    let rest: Vec<String> = arguments.collect();
    let mut seconds = 0u64;
    let stream = match how.as_str() {
        "run" => {
            let rest = CString::new(rest.join("\n")).unwrap();
            reskate_spotify::stream::reskate_stream_play(first.as_ptr(), rest.as_ptr(), sound, ended)
        }
        "file" => reskate_spotify::stream::reskate_stream_play_file(first.as_ptr(), sound, ended),
        _ => {
            seconds = rest.first().and_then(|text| text.parse().ok()).unwrap_or(8);
            reskate_spotify::stream::reskate_stream_play_url(first.as_ptr(), sound, ended, titled)
        }
    };
    println!("began stream {stream}");
    let started = std::time::Instant::now();
    while !DONE.load(Ordering::Relaxed) {
        std::thread::sleep(std::time::Duration::from_millis(50));
        if seconds > 0 && started.elapsed().as_secs() >= seconds {
            reskate_spotify::stream::reskate_stream_stop();
            seconds = 0;
        }
    }
    let (frames, rate) = (FRAMES.load(Ordering::Relaxed), RATE.load(Ordering::Relaxed));
    println!("{frames} frames at {rate} Hz: {:.1} s of sound", frames as f64 / rate.max(1) as f64);
}
