/* ReSkate native plugin API, version 1.
 *
 * A plugin is a DLL placed in a "Plugins" folder beside Skate.exe. It is loaded on the first client
 * tick of a session in which loose files are allowed (the same condition that gates custom Lua
 * scripts). Plain C, so it can be built with any compiler and language.
 *
 * Threading: every call into the plugin, and every call the plugin makes into the host, happens on
 * the game's client update tick. Do not keep the ReSkatePluginApi pointer on another thread.
 * Calls that need a live local skater return 0 / do nothing when there is none.
 *
 * Quaternions are x, y, z, w. Positions are metres in world space. Bone poses are parent-local, in
 * the board rig's own order (index 0 first).
 */
#ifndef RESKATE_PLUGIN_H
#define RESKATE_PLUGIN_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RESKATE_PLUGIN_API_VERSION 1u
#define RESKATE_MAX_PIECES 16u

typedef struct ReSkateTransform {
    float position[3];
    float rotation[4];
    float scale[3];
} ReSkateTransform;

typedef struct ReSkateSkater {
    int32_t on_map;        /* a local skater is in the world */
    float position[3];
    uint32_t physics_state;
} ReSkateSkater;

/* The skater's most recent landing, as the trainer measures it (jumps of a quarter second or more). */
typedef struct ReSkateLanding {
    uint64_t serial;       /* changes with each new landing; 0 = none yet */
    float fall_height;     /* metres from the top of the jump to the landing */
    float speed;           /* landing speed, metres per second */
    float position[3];
} ReSkateLanding;

typedef enum ReSkateLogLevel { RESKATE_LOG_DEBUG = 0, RESKATE_LOG_INFO = 1, RESKATE_LOG_WARNING = 2, RESKATE_LOG_ERROR = 3 } ReSkateLogLevel;

typedef struct ReSkatePluginApi {
    uint32_t size;         /* sizeof(ReSkatePluginApi) of the host; only use members inside it */
    uint32_t version;      /* RESKATE_PLUGIN_API_VERSION of the host */

    /* Writes to the ReSkate log. `text` is UTF-8. */
    void (*log)(int32_t level, const char* text);
    /* The directory holding Skate.exe, UTF-8, NUL-terminated. Returns the length needed (without NUL). */
    size_t (*game_directory)(char* buffer, size_t capacity);

    /* Fills `out`. Returns 0 (and zeroes `out`) when there is no local skater. */
    int32_t (*skater)(ReSkateSkater* out);
    /* Landings newer than *cursor: fills `out`, advances *cursor to its serial, returns 1. Start the cursor at
     * the value you get from the first call (or 0 to receive the current one). Returns 0 when nothing is new. */
    int32_t (*next_landing)(uint64_t* cursor, ReSkateLanding* out);
    /* Asks the game to make the local skater bail. Call again on following ticks until skater().physics_state
     * says wipeout; the game can clear a single request. Returns 0 if it could not be requested. */
    int32_t (*force_bail)(void);

    /* The local board's world transform and bone pose. `bones` gets up to `capacity` parent-local transforms
     * (bone 0 first); *count is set to the number the rig has. Returns 0 when unavailable. */
    int32_t (*board_pose)(ReSkateTransform* root, ReSkateTransform* bones, size_t capacity, size_t* count);
    /* Shows or hides the local board's meshes. The host shows it again on respawn, teleport, a skater change,
     * or when the plugin is unloaded or faults. */
    int32_t (*board_set_visible)(int32_t visible);

    /* Extra skateboard entities for effects. Created with the local board's appearance, no physics.
     * Returns a handle (non-zero) or 0. At most RESKATE_MAX_PIECES exist at once. */
    uint32_t (*piece_create)(const ReSkateTransform* at);
    int32_t (*piece_place)(uint32_t handle, const ReSkateTransform* at);
    /* Overrides the piece's bone pose (parent-local, bone 0 first). Only the deck, truck and wheel bones are used. */
    int32_t (*piece_set_bones)(uint32_t handle, const ReSkateTransform* bones, size_t count);
    void (*piece_destroy)(uint32_t handle);

    /* Plays a PCM .wav file asynchronously. `path_utf8` may be absolute or relative to the game directory. */
    int32_t (*play_sound)(const char* path_utf8);

    /* A bar in the bottom-left of the screen, in the game's menu style. `id` names it (one bar per id);
     * `fraction` is 0..1; `rgb` is 0xRRGGBB, or 0 for green, amber and red as it falls. Call it every tick
     * while it should show: a bar not refreshed for a second disappears. Returns 0 on bad input. */
    int32_t (*hud_bar)(const char* id, const char* label, float fraction, uint32_t rgb);
    void (*hud_clear)(const char* id);
} ReSkatePluginApi;

/* Exports of a plugin DLL.
 *   int  reskate_plugin_init(const ReSkatePluginApi* api);   required; return non-zero on success
 *   void reskate_plugin_tick(void);                          optional; once per client tick
 *   uint32_t reskate_plugin_api_version(void);               optional; the version it was built against
 * `api` stays valid until the process ends; plugins are never unloaded. A plugin that faults is unloaded for the session. */
typedef int32_t (*ReSkatePluginInit)(const ReSkatePluginApi*);
typedef void (*ReSkatePluginTick)(void);
typedef uint32_t (*ReSkatePluginApiVersion)(void);

#ifdef __cplusplus
}
#endif
#endif
