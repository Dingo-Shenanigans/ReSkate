//! Sound that is not Spotify's, decoded and handed to the game as it is to be heard
//! (Extension/Music/youtube_music.cpp). One stream at a time, from one of:
//!   a program's output   yt-dlp is run this way for YouTube Music
//!   a file               the player's own music (MP3, M4A, Ogg, FLAC, WAV)
//!   an address           an internet radio station (MP3 or AAC as it comes)

use std::ffi::{c_char, CStr, CString};
use std::io::Read;
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

use symphonia::core::audio::SampleBuffer;
use symphonia::core::codecs::{DecoderOptions, CODEC_TYPE_NULL};
use symphonia::core::formats::FormatOptions;
use symphonia::core::io::{MediaSource, MediaSourceStream, ReadOnlySource};
use symphonia::core::meta::{MetadataOptions, MetadataRevision, StandardTagKey};
use symphonia::core::probe::Hint;

/// The sound: frames of two floats, at `rate` Hz. It returns when the game has room for more.
pub type Sound = extern "C" fn(samples: *const f32, frames: u32, rate: u32);
/// How a stream came to its end: "ended", or "failed <why>".
pub type Ended = extern "C" fn(stream: u64, how: *const c_char);
/// What a radio station says it is playing, each time it says so.
pub type Titled = extern "C" fn(stream: u64, title: *const c_char);

/// The stream that is wanted: one that is not it stops at its next piece of sound.
static WANTED: AtomicU64 = AtomicU64::new(0);

fn decode(source: Box<dyn MediaSource>, extension: Option<&str>, stream: u64, sound: Sound) -> Result<(), String> {
    let media = MediaSourceStream::new(source, Default::default());
    let mut hint = Hint::new();
    if let Some(extension) = extension {
        hint.with_extension(extension);
    }
    let probed = symphonia::default::get_probe()
        .format(&hint, media, &FormatOptions::default(), &MetadataOptions::default())
        .map_err(|error| format!("it is no sound this can read ({error})"))?;
    let mut format = probed.format;
    let track = format.tracks().iter().find(|track| track.codec_params.codec != CODEC_TYPE_NULL).ok_or("it has no sound in it")?;
    let track_id = track.id;
    let mut decoder = symphonia::default::get_codecs()
        .make(&track.codec_params, &DecoderOptions::default())
        .map_err(|error| format!("its sound is of a kind this cannot decode ({error})"))?;
    let mut stereo: Vec<f32> = Vec::new();
    let mut samples: Option<SampleBuffer<f32>> = None;
    loop {
        if WANTED.load(Ordering::Relaxed) != stream {
            return Ok(());
        }
        let packet = match format.next_packet() {
            Ok(packet) => packet,
            // (The end of the file, or of what the program or the station sent.)
            Err(symphonia::core::errors::Error::IoError(_)) => return Ok(()),
            Err(symphonia::core::errors::Error::ResetRequired) => return Ok(()),
            Err(error) => return Err(error.to_string()),
        };
        if packet.track_id() != track_id {
            continue;
        }
        let decoded = match decoder.decode(&packet) {
            Ok(decoded) => decoded,
            // (A piece that cannot be decoded is passed over.)
            Err(symphonia::core::errors::Error::DecodeError(_)) => continue,
            Err(error) => return Err(error.to_string()),
        };
        let spec = *decoded.spec();
        let channels = spec.channels.count().max(1);
        // (A piece with more in it than the last: room is made for it.)
        if samples.as_ref().is_some_and(|buffer| buffer.capacity() < decoded.capacity() * channels) {
            samples = None;
        }
        let buffer = samples.get_or_insert_with(|| SampleBuffer::<f32>::new(decoded.capacity() as u64, spec));
        buffer.copy_interleaved_ref(decoded);
        let interleaved = buffer.samples();
        let frames = interleaved.len() / channels;
        stereo.clear();
        for frame in 0..frames {
            let left = interleaved[frame * channels];
            stereo.push(left);
            stereo.push(if channels > 1 { interleaved[frame * channels + 1] } else { left });
        }
        if frames > 0 {
            sound(stereo.as_ptr(), frames as u32, spec.rate);
        }
    }
}

fn text(pointer: *const c_char) -> Option<String> {
    if pointer.is_null() {
        return None;
    }
    // SAFETY: the game passes a string that ends in a zero and outlives the call.
    unsafe { CStr::from_ptr(pointer) }.to_str().ok().map(str::to_string)
}

/// Runs `work` as the stream that is wanted from now on, on a thread of its own, and tells
/// `ended` how it came out. Returns the stream's number, or 0 when it could not be begun.
fn begin(ended: Ended, work: impl FnOnce(u64) -> Result<(), String> + Send + 'static) -> u64 {
    let stream = WANTED.fetch_add(1, Ordering::Relaxed) + 1;
    let begun = std::thread::Builder::new().name("ReSkate stream".to_string()).spawn(move || {
        // (Nothing that goes wrong in here is to take the game with it.)
        let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(move || work(stream)));
        let how = match outcome {
            Ok(Ok(())) => "ended".to_string(),
            Ok(Err(why)) => format!("failed {why}"),
            Err(_) => "failed on a fault of its own".to_string(),
        };
        if let Ok(how) = CString::new(how.replace('\0', " ")) {
            ended(stream, how.as_ptr());
        }
    });
    if begun.is_ok() {
        stream
    } else {
        0
    }
}

/// Runs `program` with `arguments` (one to a line) and plays what it writes, in the place of
/// any stream before it.
#[no_mangle]
pub extern "C" fn reskate_stream_play(program: *const c_char, arguments: *const c_char, sound: Sound, ended: Ended) -> u64 {
    let (Some(program), Some(arguments)) = (text(program), text(arguments)) else { return 0 };
    begin(ended, move |stream| {
        let mut command = Command::new(&program);
        command.args(arguments.lines().filter(|line| !line.is_empty())).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::null());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
        }
        let mut child = command.spawn().map_err(|error| format!("{program} could not be run ({error})"))?;
        let output = child.stdout.take().ok_or("it has no output")?;
        let result = decode(Box::new(ReadOnlySource::new(Watched { inner: output, stream })), None, stream, sound);
        let _ = child.kill();
        let _ = child.wait();
        result
    })
}

/// Plays a file of the player's.
#[no_mangle]
pub extern "C" fn reskate_stream_play_file(path: *const c_char, sound: Sound, ended: Ended) -> u64 {
    let Some(path) = text(path) else { return 0 };
    begin(ended, move |stream| {
        let file = std::fs::File::open(&path).map_err(|error| format!("the file could not be opened ({error})"))?;
        let extension = std::path::Path::new(&path).extension().and_then(|extension| extension.to_str()).map(str::to_lowercase);
        decode(Box::new(file), extension.as_deref(), stream, sound)
    })
}

/// What a file says of itself in its tags: its artist, its title, and its picture.
fn about(path: &str) -> Option<(String, String, Vec<u8>)> {
    let file = std::fs::File::open(path).ok()?;
    let mut hint = Hint::new();
    if let Some(extension) = std::path::Path::new(path).extension().and_then(|extension| extension.to_str()) {
        hint.with_extension(&extension.to_lowercase());
    }
    let media = MediaSourceStream::new(Box::new(file), Default::default());
    let mut probed = symphonia::default::get_probe().format(&hint, media, &FormatOptions::default(), &MetadataOptions::default()).ok()?;
    let (mut artist, mut title, mut picture) = (String::new(), String::new(), Vec::new());
    let mut take = |revision: &MetadataRevision| {
        for tag in revision.tags() {
            match tag.std_key {
                Some(StandardTagKey::Artist) if artist.is_empty() => artist = tag.value.to_string(),
                Some(StandardTagKey::TrackTitle) if title.is_empty() => title = tag.value.to_string(),
                _ => {}
            }
        }
        if picture.is_empty() {
            if let Some(visual) = revision.visuals().first() {
                picture = visual.data.to_vec();
            }
        }
    };
    {
        let metadata = probed.format.metadata();
        if let Some(revision) = metadata.current() {
            take(revision);
        }
    }
    if let Some(metadata) = probed.metadata.get() {
        if let Some(revision) = metadata.current() {
            take(revision);
        }
    }
    Some((artist, title, picture))
}

/// Writes `text` where the game gave room for it, ended with a zero, and whole characters only.
fn write(text: &str, to: *mut c_char, room: u32) {
    if to.is_null() || room == 0 {
        return;
    }
    let mut most = text.len().min(room as usize - 1);
    while !text.is_char_boundary(most) {
        most -= 1;
    }
    // SAFETY: the game gave `room` bytes at `to`.
    unsafe {
        std::ptr::copy_nonoverlapping(text.as_ptr(), to.cast::<u8>(), most);
        *to.add(most) = 0;
    }
}

/// A file's artist and title (each into `text_room` bytes; empty when it does not say), and
/// its picture when room is given for one. Returns the picture's size, or 0 when it has none
/// or it is larger than the room.
#[no_mangle]
pub extern "C" fn reskate_file_about(path: *const c_char, artist: *mut c_char, title: *mut c_char, text_room: u32, picture: *mut u8, picture_room: u32) -> u32 {
    write("", artist, text_room);
    write("", title, text_room);
    let Some(path) = text(path) else { return 0 };
    let Ok(Some((its_artist, its_title, its_picture))) = std::panic::catch_unwind(move || about(&path)) else { return 0 };
    write(its_artist.replace(' ', " ").trim(), artist, text_room);
    write(its_title.replace(' ', " ").trim(), title, text_room);
    if picture.is_null() || its_picture.is_empty() || its_picture.len() > picture_room as usize {
        return 0;
    }
    // SAFETY: the game gave `picture_room` bytes at `picture`.
    unsafe { std::ptr::copy_nonoverlapping(its_picture.as_ptr(), picture, its_picture.len()) };
    its_picture.len() as u32
}

/// What a station sends, with what it says of itself taken out of the sound: every so many
/// bytes (it says how many) there is a note of what is playing ("StreamTitle='...';").
struct Station<R: Read> {
    inner: R,
    every: usize,
    left: usize,
    stream: u64,
    titled: Titled,
    last: String,
}
impl<R: Read> Station<R> {
    fn note(&mut self) -> std::io::Result<()> {
        let mut length = [0u8; 1];
        self.inner.read_exact(&mut length)?;
        let mut note = vec![0u8; usize::from(length[0]) * 16];
        self.inner.read_exact(&mut note)?;
        let note = String::from_utf8_lossy(&note);
        if let Some(from) = note.find("StreamTitle='") {
            let rest = &note[from + 13..];
            let title = rest.find("';").map_or(rest, |end| &rest[..end]).trim_matches('\0').trim().to_string();
            if title != self.last {
                self.last = title.clone();
                if let Ok(title) = CString::new(title.replace('\0', " ")) {
                    (self.titled)(self.stream, title.as_ptr());
                }
            }
        }
        Ok(())
    }
}
impl<R: Read> Read for Station<R> {
    fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
        if self.every == 0 {
            return self.inner.read(buffer);
        }
        if self.left == 0 {
            self.note()?;
            self.left = self.every;
        }
        let most = buffer.len().min(self.left);
        let got = self.inner.read(&mut buffer[..most])?;
        self.left -= got;
        Ok(got)
    }
}

/// A reader that comes to its end once its stream is no longer the one wanted, so that one
/// which is waiting on a station or a program is not waited on in turn.
struct Watched<R: Read> {
    inner: R,
    stream: u64,
}
impl<R: Read> Read for Watched<R> {
    fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
        if WANTED.load(Ordering::Relaxed) != self.stream {
            return Ok(0);
        }
        self.inner.read(buffer)
    }
}

/// AAC as stations send it (ADTS), from the start of a frame, and with each frame marked as
/// the decoder knows it: it takes only the mark of MPEG-4 (ff f1), and many stations send
/// that of MPEG-2 (ff f9), which is the same sound (seen 2026-10-12, SomaFM).
struct Adts<R: Read> {
    inner: R,
    pending: Vec<u8>,
    pass: usize,
}
impl<R: Read> Read for Adts<R> {
    fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
        loop {
            if self.pass > 0 && !self.pending.is_empty() {
                let count = buffer.len().min(self.pass).min(self.pending.len());
                buffer[..count].copy_from_slice(&self.pending[..count]);
                self.pending.drain(..count);
                self.pass -= count;
                return Ok(count);
            }
            if self.pass == 0 && self.pending.len() >= 7 {
                let header = &mut self.pending[..7];
                let length = (usize::from(header[3] & 3) << 11) | (usize::from(header[4]) << 3) | usize::from(header[5] >> 5);
                if header[0] == 0xff && header[1] & 0xf6 == 0xf0 && length >= 7 {
                    header[1] &= !0x08;
                    self.pass = length;
                } else {
                    self.pending.remove(0); // (not the start of a frame: the next byte is tried)
                }
                continue;
            }
            let mut more = [0u8; 4096];
            let got = self.inner.read(&mut more)?;
            if got == 0 {
                return Ok(0);
            }
            self.pending.extend_from_slice(&more[..got]);
        }
    }
}

/// Plays what is at an address as it comes: an internet radio station. An address that is a
/// list of addresses (.m3u, .pls) is followed to its first.
#[no_mangle]
pub extern "C" fn reskate_stream_play_url(address: *const c_char, sound: Sound, ended: Ended, titled: Titled) -> u64 {
    let Some(mut address) = text(address) else { return 0 };
    begin(ended, move |stream| {
        let client = reqwest::blocking::Client::builder()
            .connect_timeout(Duration::from_secs(10))
            .timeout(None::<Duration>) // (a station has no end)
            .user_agent("ReSkate")
            .build()
            .map_err(|error| error.to_string())?;
        for _ in 0..3 {
            let response = client.get(&address).header("Icy-MetaData", "1").send().map_err(|error| format!("the station could not be reached ({error})"))?;
            if !response.status().is_success() {
                return Err(format!("the station answered {}", response.status().as_u16()));
            }
            let kind = response.headers().get("content-type").and_then(|value| value.to_str().ok()).unwrap_or("").to_lowercase();
            let path = address.split(['?', '#']).next().unwrap_or("").to_lowercase();
            if kind.contains("mpegurl") || kind.contains("scpls") || path.ends_with(".m3u") || path.ends_with(".pls") || path.ends_with(".m3u8") {
                let mut list = String::new();
                response.take(64 * 1024).read_to_string(&mut list).map_err(|error| error.to_string())?;
                if list.contains("#EXT-X-") {
                    return Err("the station sends its sound in a way this does not play (HLS)".to_string());
                }
                let next = list.lines().map(str::trim).find_map(|line| {
                    let line = line.split_once('=').filter(|(key, _)| key.to_lowercase().starts_with("file")).map_or(line, |(_, value)| value);
                    (line.starts_with("http://") || line.starts_with("https://")).then(|| line.to_string())
                });
                address = next.ok_or("the station's list has no address in it")?;
                continue;
            }
            let every = response.headers().get("icy-metaint").and_then(|value| value.to_str().ok()).and_then(|value| value.parse::<usize>().ok()).unwrap_or(0);
            let extension = if kind.contains("aac") { Some("aac") } else if kind.contains("ogg") { Some("ogg") } else if kind.contains("mpeg") { Some("mp3") } else { None };
            let station = Watched { inner: Station { inner: response, every, left: every, stream, titled, last: String::new() }, stream };
            return if extension == Some("aac") {
                decode(Box::new(ReadOnlySource::new(Adts { inner: station, pending: Vec::new(), pass: 0 })), extension, stream, sound)
            } else {
                decode(Box::new(ReadOnlySource::new(station)), extension, stream, sound)
            };
        }
        Err("the station's address led round and round".to_string())
    })
}

/// Stops the stream that is playing, if any.
#[no_mangle]
pub extern "C" fn reskate_stream_stop() {
    WANTED.fetch_add(1, Ordering::Relaxed);
}
