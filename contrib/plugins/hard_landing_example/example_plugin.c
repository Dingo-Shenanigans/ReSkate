/* Example ReSkate plugin: after a hard landing the board is replaced by a copy that tumbles to the
 * ground for three seconds, then the real board comes back. Shows the landing, board, piece and
 * bail calls. Build (x64 MSVC prompt):
 *   cl /LD /O2 example_plugin.c /Fe:example_plugin.dll
 * Put the dll in a "Plugins" folder beside Skate.exe. */
#include <math.h>
#include <string.h>
#include <windows.h>
#include "../../../Extension/Plugins/reskate_plugin.h"

static const ReSkatePluginApi* host;
static uint64_t cursor, started;
static uint32_t piece;
static ReSkateTransform where;
static float vy, spin;
static int bailing;

__declspec(dllexport) uint32_t reskate_plugin_api_version(void) { return 1; }

__declspec(dllexport) int32_t reskate_plugin_init(const ReSkatePluginApi* api) {
    if (api->version < 1 || api->size < sizeof(ReSkatePluginApi)) return 0;
    host = api;
    ReSkateLanding first;
    host->next_landing(&cursor, &first); /* ignore the landing that is already there */
    host->log(RESKATE_LOG_INFO, "example plugin ready: land from 2 m or more");
    return 1;
}

__declspec(dllexport) void reskate_plugin_tick(void) {
    const uint64_t now = GetTickCount64();
    if (piece) {
        if (bailing) {
            ReSkateSkater s;
            host->skater(&s);
            if (s.physics_state == 300 || !host->force_bail()) bailing = 0;
        }
        vy -= 9.8f * 0.016f;
        where.position[1] += vy * 0.016f;
        const float half = 0.5f * spin * 0.016f; /* turn about the vertical axis */
        const float c = cosf(half), sn = sinf(half);
        const float q[4] = {0, sn, 0, c}, r[4] = {where.rotation[0], where.rotation[1], where.rotation[2], where.rotation[3]};
        where.rotation[0] = q[3] * r[0] + q[0] * r[3] + q[1] * r[2] - q[2] * r[1];
        where.rotation[1] = q[3] * r[1] - q[0] * r[2] + q[1] * r[3] + q[2] * r[0];
        where.rotation[2] = q[3] * r[2] + q[0] * r[1] - q[1] * r[0] + q[2] * r[3];
        where.rotation[3] = q[3] * r[3] - q[0] * r[0] - q[1] * r[1] - q[2] * r[2];
        if (!host->piece_place(piece, &where) || now - started > 3000) {
            host->piece_destroy(piece);
            host->board_set_visible(1);
            piece = 0;
        }
        return;
    }
    ReSkateLanding landing;
    if (!host->next_landing(&cursor, &landing) || landing.fall_height < 2.0f) return;
    ReSkateTransform root, bones[32];
    size_t count = 0;
    if (!host->board_pose(&root, bones, 32, &count)) return;
    if (!host->board_set_visible(0)) return;
    where = root;
    piece = host->piece_create(&where);
    if (!piece) { host->board_set_visible(1); return; }
    started = now;
    vy = 2.0f;
    spin = 6.0f;
    bailing = 1;
}
