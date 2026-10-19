#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <cstddef>
#include <cstdint>

// The replay editor's camera keyframes (Extension/ReplayKeyframes/replay_keyframes.h): framing keys
// (Behaviour, Subject, offsets, FOV), the free camera's among them, played as cubic Hermite segments
// between neighbouring keys. (The camera's other track, whole transforms played by 0x6222e0, isn't
// used by the free camera and is left alone.)
namespace dingosdk::game::build::v20260929::replay_keyframes {
// ---- Framing track: segments built when keys change ----
// rebuild(manager, session): any key edit ends here. It builds a segment for every framing key of the
// session with segment_build and sends the lot, with the transform keys, to the camera object. The
// manager sends one package at a time (its handle is at manager + 0x40): the session it rebuilt last
// is the one the camera plays.
inline constexpr Fingerprint session_rebuild{0x62c500, {
    0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x57,0x41,0x57,0x48,0x8d,
    0xac,0x24,0x08,0xfe,0xff,0xff,0x48,0x81,0xec,0xf8,0x02,0x00,0x00,0x4c,0x8b,0xf9}};
// Segment* segment_build(manager, Segment* out, Node* keys_end, Node** key): the segment from a key
// to the next. keys_end is the anchor of the session's key map (session node + 0x60: [1] is the first
// key); the session's id is the int at session node + 0x20.
inline constexpr Fingerprint segment_build{0x60c7a0, {
    0x40,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xf0,
    0x00,0x00,0x00,0x4c,0x8b,0xe1,0x4d,0x8b,0xf9,0x48,0x8b,0xca,0x4d,0x8b,0xe8,0x48}};
inline constexpr std::size_t session_id_before_keys = 0x60 - 0x20;
// void slot_build(manager, const void* value, double time, int behaviour, int subject, Slot* out):
// one key as a segment slot, under a Behaviour and Subject (segment_build passes the segment's own).
inline constexpr Fingerprint slot_build{0x60ec20, {
    0x4c,0x8b,0xdc,0x49,0x89,0x73,0x10,0x57,0x48,0x81,0xec,0x60,0x01,0x00,0x00,0x48,
    0x8b,0x82,0xf0,0x00,0x00,0x00,0x48,0x8b,0xfa,0x48,0x8b,0xb4,0x24,0x98,0x01,0x00}};
// void slot_unwrap(Slot* adjust, const Slot* reference, int behaviour): adjust's angles to within π
// of the reference's.
inline constexpr Fingerprint slot_unwrap{0x61abd0, {
    0x48,0x83,0xec,0x48,0x45,0x85,0xc0,0x75,0x6d,0xc5,0xfa,0x10,0x52,0x48,0xc5,0xfa,
    0x10,0x49,0x48,0xc5,0xf2,0x5c,0xc2,0xc5,0xf8,0x54,0x05,0xf1,0xb9,0xa5,0x05,0xc5}};
// bool segment_eval(camera, double time, const Segment*, Out*): plays a framing segment (every frame;
// not hooked: what it plays is decided when the segment is built), every lane through the game's
// spline (p0, p1, p2, p3, s, k1, k2):
//   m1 = (1 − k1)·0.5·(p2 − p0), m2 = (1 − k2)·0.5·(p3 − p1), Hermite from p1 to p2.
inline constexpr Fingerprint segment_eval{0x6220d0, {
    0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x57,0x48,0x81,0xec,0x50,0x01,0x00,0x00,0xc5,
    0xfc,0x10,0x05,0xe9,0x15,0xdb,0x05,0xc5,0xfc,0x10,0x15,0x01,0x16,0xdb,0x05,0x41}};
// A segment this short or shorter plays as its end (s = 1).
inline constexpr double tiny_gap = 1.0 / 65536.0;
// The manager singleton.
inline constexpr std::uintptr_t manager = 0x71f2130;
inline constexpr std::size_t manager_sessions = 0x10; // EASTL map<int, Session>: anchor; root at +0x20

// A key's node in a session's framing map: time at + 0x20, value at + 0x30.
inline constexpr std::size_t framing_key_time = 0x20;
inline constexpr std::size_t framing_key_value = 0x30;

// The segment (0x1e8 bytes): four slots of 0x60 bytes, two blocks of twelve k, Behaviour, Subject.
inline constexpr std::size_t slot_size = 0x60;
inline constexpr std::size_t slot_floats = slot_size / sizeof(float);
inline constexpr std::size_t slot_time = 0x40; // double
inline constexpr std::size_t segment_next = 0x000;     // p3, rescaled to one segment's time away
inline constexpr std::size_t segment_to = 0x060;       // p2
inline constexpr std::size_t segment_previous = 0x0c0; // p0, rescaled likewise
inline constexpr std::size_t segment_from = 0x120;     // p1
inline constexpr std::size_t segment_k_to = 0x180;     // twelve floats: the k of the to key
inline constexpr std::size_t segment_k_from = 0x1b0;   // and of the from key
inline constexpr std::size_t segment_k_count = 12;
inline constexpr std::size_t segment_behaviour = 0x1e0;
// The free camera's keys play under this Behaviour (with Subject 0): seen in game, 2026-10-16.
inline constexpr int free_camera_behaviour = 1;
inline constexpr std::size_t segment_subject = 0x1e4;
inline constexpr std::size_t segment_size = 0x1e8;

// Which slot floats each Behaviour plays, and with which k (segment_eval's evaluators 0x609a50,
// 0x608e10 and 0x609fe0, plus the FOV lane segment_eval plays itself).
struct Lane {
    unsigned char slot_float, k;
};
inline constexpr Lane behaviour_0_lanes[]{{0, 0}, {1, 1}, {2, 2}, {8, 4}, {9, 5}, {10, 6}, {18, 8}, {19, 9}, {21, 10}};
inline constexpr Lane behaviour_1_lanes[]{{12, 0}, {13, 1}, {14, 2}, {4, 4}, {5, 5}, {6, 6}, {21, 10}};
inline constexpr Lane behaviour_2_lanes[]{{0, 0}, {1, 1}, {2, 2}, {4, 4}, {5, 5}, {6, 6}, {18, 8}, {19, 9}, {21, 10}};
}
