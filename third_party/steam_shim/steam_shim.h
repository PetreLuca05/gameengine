// Minimal extern "C" API over the Steamworks SDK (C++), used by src/net/transport_steam.c.
// Compiled only when STEAM_SDK_DIR is configured (DG_HAVE_STEAM).
#ifndef DG_STEAM_SHIM_H
#define DG_STEAM_SHIM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DGS_EV_CONNECTED = 1,      // conn
    DGS_EV_DISCONNECTED,       // conn
    DGS_EV_DATA,               // conn, data, size
    DGS_EV_LOBBY_CREATED,      // id = lobby
    DGS_EV_LOBBY_ENTERED,      // id = lobby, owner = host steam id
    DGS_EV_JOIN_REQUESTED,     // id = lobby the user asked to join (friends list / invite)
};

typedef struct DgsEvent {
    int type;
    uint32_t conn;
    uint64_t id;
    uint64_t owner;
    const void *data;
    uint32_t size;
} DgsEvent;

bool dgs_init(void);
void dgs_shutdown(void);
void dgs_run_callbacks(void);
bool dgs_available(void);
uint64_t dgs_my_id(void);
const char *dgs_persona_name(void);

bool dgs_listen(void);
bool dgs_connect(uint64_t steam_id);
bool dgs_send(uint32_t conn, const void *data, uint32_t size, bool reliable);
void dgs_close(uint32_t conn);
int dgs_poll(DgsEvent *out, int max);   // data pointers valid until the next dgs_poll
bool dgs_stats(uint32_t conn, float *ping_ms, float *loss_pct);

bool dgs_create_lobby(int max_members);
bool dgs_join_lobby(uint64_t lobby);
void dgs_leave_lobby(void);
uint64_t dgs_current_lobby(void);
void dgs_invite_overlay(void);
void dgs_set_rich_presence(const char *key, const char *value);

#ifdef __cplusplus
}
#endif
#endif
