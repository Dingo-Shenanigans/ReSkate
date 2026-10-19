//! ReSkate's Spotify Connect device (librespot), built into ReSkate.dll.
//!
//! The game is a device in the player's Spotify app ("ReSkate"), and what Spotify plays on it
//! goes to the game (Extension/Music/spotify_device.cpp), which starts this once:
//!   `sound`  is given the sound as it is to be heard: 32-bit float samples, stereo, 44100 Hz.
//!            It returns when the game has room for more, which is what keeps Spotify at the
//!            speed of the game.
//!   `told`   is given a line for each thing the game is told, with tabs between its parts:
//!              READY                      the device is in Spotify's list (on this network)
//!              CONNECTED <user>           a Spotify account is using it
//!              DISCONNECTED
//!              TRACK <uri> <artist> <title> <cover address> <milliseconds>
//!              STATE playing|paused|stopped
//!              LOG <text>                 this library's own log
//!              ENDED                      it has stopped for good
//!   `reskate_spotify_ask` takes a line for each thing the game asks:
//!              activate                   play here, taking over from wherever Spotify plays
//!              play | pause | next | prev
//!              load <context uri> [<track uri>]     a playlist or album, at a track of it
//!              shuffle <context uri>                the same, in no order and over again
//!              tracks <index> <track uri>...        these tracks, at one of them
//!              quit
//!
//! Who the account is comes from the Spotify app: picking the device there hands it over, and
//! it is kept (the cache folder) so the device is there the next time without being picked.

pub mod stream;

use std::ffi::{c_char, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;
use std::sync::{Arc, OnceLock};
use std::time::Duration;

use futures_util::StreamExt;
use librespot_connect::{ConnectConfig, LoadContextOptions, LoadRequest, LoadRequestOptions, Options, PlayingTrack, Spirc};
use librespot_core::authentication::Credentials;
use librespot_core::cache::Cache;
use librespot_core::config::SessionConfig;
use librespot_core::error::ErrorKind;
use librespot_core::session::Session;
use librespot_discovery::{DeviceType, Discovery};
use librespot_metadata::audio::UniqueFields;
use librespot_playback::audio_backend::{Sink, SinkError, SinkResult};
use librespot_playback::config::{Bitrate, PlayerConfig};
use librespot_playback::convert::Converter;
use librespot_playback::decoder::AudioPacket;
use librespot_playback::mixer::softmixer::SoftMixer;
use librespot_playback::mixer::{Mixer, MixerConfig};
use librespot_playback::player::{Player, PlayerEvent};
use sha1::{Digest, Sha1};
use tokio::sync::mpsc;

type Sound = extern "C" fn(samples: *const f32, frames: u32);
type Told = extern "C" fn(line: *const c_char);

static SOUND: OnceLock<Sound> = OnceLock::new();
static TOLD: OnceLock<Told> = OnceLock::new();
static ASK: OnceLock<mpsc::UnboundedSender<String>> = OnceLock::new();

/// What the game is told: a line.
fn tell(parts: &[&str]) {
    let clean: Vec<String> = parts.iter().map(|part| part.replace(['\t', '\r', '\n', '\0'], " ")).collect();
    if let (Some(told), Ok(line)) = (TOLD.get(), CString::new(clean.join("\t"))) {
        told(line.as_ptr());
    }
}

/// This library's own log, told to the game as well.
struct Log;
impl log::Log for Log {
    fn enabled(&self, metadata: &log::Metadata) -> bool {
        metadata.level() <= log::Level::Info
    }
    fn log(&self, record: &log::Record) {
        if self.enabled(record.metadata()) {
            tell(&["LOG", &format!("{}: {}", record.level(), record.args())]);
        }
    }
    fn flush(&self) {}
}

/// The sound goes to the game as it comes.
struct GameSink;
impl Sink for GameSink {
    fn write(&mut self, packet: AudioPacket, converter: &mut Converter) -> SinkResult<()> {
        let samples = packet.samples().map_err(|error| SinkError::OnWrite(error.to_string()))?;
        let converted = converter.f64_to_f32(samples);
        if let Some(sound) = SOUND.get() {
            sound(converted.as_ptr(), (converted.len() / 2) as u32);
        }
        Ok(())
    }
}

fn told(event: PlayerEvent) {
    match event {
        PlayerEvent::TrackChanged { audio_item } => {
            let artist = match &audio_item.unique_fields {
                UniqueFields::Track { artists, .. } => artists.first().map(|artist| artist.name.clone()).unwrap_or_default(),
                UniqueFields::Episode { show_name, .. } => show_name.clone(),
                _ => String::new(),
            };
            let cover = audio_item.covers.iter().max_by_key(|cover| cover.width).map(|cover| cover.url.clone()).unwrap_or_default();
            tell(&["TRACK", &audio_item.uri, &artist, &audio_item.name, &cover, &audio_item.duration_ms.to_string()]);
        }
        PlayerEvent::Playing { .. } => tell(&["STATE", "playing"]),
        PlayerEvent::Paused { .. } => tell(&["STATE", "paused"]),
        PlayerEvent::Stopped { .. } => tell(&["STATE", "stopped"]),
        PlayerEvent::SessionConnected { user_name, .. } => tell(&["CONNECTED", &user_name]),
        PlayerEvent::SessionDisconnected { .. } => tell(&["DISCONNECTED"]),
        _ => {}
    }
}

async fn run(name: String, cache_folder: Option<PathBuf>, mut asked: mpsc::UnboundedReceiver<String>) {
    let device_id = hex::encode(Sha1::digest(name.as_bytes()));
    let mut session_config = SessionConfig::default();
    session_config.device_id = device_id.clone();
    let cache = cache_folder.and_then(|folder| Cache::new(Some(folder.clone()), Some(folder), None::<PathBuf>, None).ok());

    // In Spotify's list of devices on this network: picking it there hands over the account.
    let mut discovery = match Discovery::builder(device_id, session_config.client_id.clone()).name(name.clone()).device_type(DeviceType::Computer).launch() {
        Ok(discovery) => {
            tell(&["READY"]);
            Some(discovery)
        }
        Err(error) => {
            log::error!("the device could not be put in Spotify's list: {error}");
            None
        }
    };

    let mut credentials: Option<Credentials> = cache.as_ref().and_then(|cache| cache.credentials());
    // Tries in a row that could not reach Spotify with the account (the wait grows with them).
    let mut unreachable: u32 = 0;
    loop {
        // Until there is an account: one kept from before, or one handed over now.
        let account = match credentials.take() {
            Some(account) => account,
            None => loop {
                tokio::select! {
                    found = async { match discovery.as_mut() { Some(discovery) => discovery.next().await, None => std::future::pending().await } } => {
                        if let Some(account) = found { break account; }
                    }
                    line = asked.recv() => match line.as_deref() {
                        Some("quit") | None => return,
                        _ => {}
                    },
                }
            },
        };

        let session = Session::new(session_config.clone(), cache.clone());
        let mixer: Arc<dyn Mixer> = match SoftMixer::open(MixerConfig::default()) {
            Ok(mixer) => Arc::new(mixer),
            Err(error) => {
                log::error!("no mixer: {error}");
                return;
            }
        };
        let player_config = PlayerConfig { bitrate: Bitrate::Bitrate320, ..PlayerConfig::default() };
        let player = Player::new(player_config, session.clone(), mixer.get_soft_volume(), || Box::new(GameSink));
        let mut events = player.get_player_event_channel();
        let connect_config = ConnectConfig { name: name.clone(), device_type: DeviceType::Computer, initial_volume: u16::MAX, ..ConnectConfig::default() };
        let kept = account.clone();
        let (spirc, task) = match Spirc::new(connect_config, session.clone(), account, player.clone(), mixer).await {
            Ok(made) => made,
            Err(error) => {
                log::error!("Spotify did not take the account: {error}");
                tell(&["DISCONNECTED"]);
                let wait = match error.kind {
                    // (An account Spotify refuses, or one not Premium: it is asked for anew.)
                    ErrorKind::Unauthenticated | ErrorKind::PermissionDenied | ErrorKind::InvalidArgument => {
                        unreachable = 0;
                        Duration::from_secs(3)
                    }
                    // Spotify could not be reached (the connection was down, its service did not
                    // answer): the same account is tried again, waiting longer each time. It used
                    // to be dropped here, and the device then waited to be picked in Spotify's
                    // own app: a game started while the connection was down played no Spotify
                    // until it was restarted (2026-10-15).
                    _ => {
                        credentials = Some(kept);
                        unreachable = unreachable.saturating_add(1);
                        let seconds = (3u64 << unreachable.saturating_sub(1).min(5)).min(60);
                        log::warn!("Spotify could not be reached; trying the account again in {seconds} s");
                        Duration::from_secs(seconds)
                    }
                };
                tokio::select! {
                    _ = tokio::time::sleep(wait) => {}
                    line = asked.recv() => if matches!(line.as_deref(), Some("quit") | None) { return; },
                }
                continue;
            }
        };
        unreachable = 0;
        // (With the account taken, the device is the player's to use.)
        tell(&["CONNECTED", &session.username()]);
        tokio::pin!(task);

        loop {
            tokio::select! {
                _ = &mut task => {
                    tell(&["DISCONNECTED"]);
                    // (The connection to Spotify ended under it: the account is taken up again,
                    // as kept since it was given, rather than waiting to be picked in Spotify's app.)
                    if credentials.is_none() {
                        credentials = cache.as_ref().and_then(|cache| cache.credentials()).or_else(|| Some(kept.clone()));
                        tokio::select! {
                            _ = tokio::time::sleep(Duration::from_secs(3)) => {}
                            line = asked.recv() => if matches!(line.as_deref(), Some("quit") | None) { return; },
                        }
                    }
                    break;
                }
                event = events.recv() => match event {
                    Some(event) => told(event),
                    None => break,
                },
                found = async { match discovery.as_mut() { Some(discovery) => discovery.next().await, None => std::future::pending().await } } => {
                    // Another account was handed over: this one is let go of.
                    if let Some(account) = found {
                        credentials = Some(account);
                        let _ = spirc.shutdown();
                    }
                }
                line = asked.recv() => {
                    let Some(line) = line else { return };
                    let mut words = line.split_whitespace();
                    let result = match words.next() {
                        Some("activate") => spirc.activate(),
                        Some("play") => spirc.play(),
                        Some("pause") => spirc.pause(),
                        Some("next") => spirc.next(),
                        Some("prev") => spirc.prev(),
                        Some("load") => match words.next() {
                            Some(context) => {
                                let playing_track = words.next().map(|track| PlayingTrack::Uri(track.to_string()));
                                // (Loading is for the device that is playing: this is made it first.)
                                let _ = spirc.activate();
                                tokio::time::sleep(Duration::from_millis(400)).await;
                                spirc.load(LoadRequest::from_context_uri(context.to_string(), LoadRequestOptions { start_playing: true, seek_to: 0, context_options: None, playing_track }))
                            }
                            None => Ok(()),
                        },
                        Some("shuffle") => match words.next() {
                            Some(context) => {
                                let _ = spirc.activate();
                                tokio::time::sleep(Duration::from_millis(400)).await;
                                let context_options = Some(LoadContextOptions::Options(Options { shuffle: true, repeat: true, repeat_track: false }));
                                spirc.load(LoadRequest::from_context_uri(context.to_string(), LoadRequestOptions { start_playing: true, seek_to: 0, context_options, playing_track: None }))
                            }
                            None => Ok(()),
                        },
                        Some("tracks") => {
                            let index = words.next().and_then(|index| index.parse::<u32>().ok()).unwrap_or(0);
                            let tracks: Vec<String> = words.map(str::to_string).collect();
                            if tracks.is_empty() {
                                Ok(())
                            } else {
                                let _ = spirc.activate();
                                tokio::time::sleep(Duration::from_millis(400)).await;
                                spirc.load(LoadRequest::from_tracks(tracks, LoadRequestOptions { start_playing: true, seek_to: 0, context_options: None, playing_track: Some(PlayingTrack::Index(index)) }))
                            }
                        }
                        Some("quit") => {
                            let _ = spirc.shutdown();
                            return;
                        }
                        _ => Ok(()),
                    };
                    if let Err(error) = result {
                        log::warn!("'{line}' was not done: {error}");
                    }
                }
            }
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

/// Starts the device, once, on threads of its own. `name` is what it is called in Spotify,
/// `cache` the folder the account is kept in (may be null). Returns 1 when it was started.
#[no_mangle]
pub extern "C" fn reskate_spotify_start(name: *const c_char, cache: *const c_char, sound: Sound, told: Told) -> i32 {
    let (ask, asked) = mpsc::unbounded_channel::<String>();
    if ASK.set(ask).is_err() {
        return 0;
    }
    let _ = SOUND.set(sound);
    let _ = TOLD.set(told);
    let _ = log::set_logger(&Log).map(|()| log::set_max_level(log::LevelFilter::Info));
    let name = text(name).unwrap_or_else(|| "ReSkate".to_string());
    let cache = text(cache).map(PathBuf::from);
    let started = std::thread::Builder::new().name("ReSkate Spotify".to_string()).spawn(move || {
        // (Nothing that goes wrong here is to take the game with it.)
        let outcome = catch_unwind(AssertUnwindSafe(|| match tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build() {
            Ok(runtime) => runtime.block_on(run(name, cache, asked)),
            Err(error) => log::error!("no runtime: {error}"),
        }));
        if outcome.is_err() {
            tell(&["LOG", "ERROR: the Spotify device stopped on a fault of its own"]);
        }
        tell(&["ENDED"]);
    });
    i32::from(started.is_ok())
}

/// Asks the device for something (see the top of this file).
#[no_mangle]
pub extern "C" fn reskate_spotify_ask(line: *const c_char) {
    if let (Some(ask), Some(line)) = (ASK.get(), text(line)) {
        let _ = ask.send(line.trim().to_string());
    }
}
