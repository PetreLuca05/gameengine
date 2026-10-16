// Steamworks shim (C++). Keeps all Steam C++ types out of the C code base.
// Written against Steamworks SDK 1.5x/1.6x (ISteamNetworkingSockets + ISteamMatchmaking).
#include "steam_shim.h"
#include "steam/steam_api.h"
#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"
#include "steam/isteammatchmaking.h"
#include "steam/isteamfriends.h"
#include <stdio.h>
#include <string.h>

#define DGS_MAX_QUEUED 256
#define DGS_MAX_MSGS 256

namespace {

struct ShimState;
ShimState *g_shim = nullptr; // owned by dgs_init/dgs_shutdown; the C side stores nothing else

struct ShimState {
    bool ok = false;
    HSteamListenSocket listen = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup group = k_HSteamNetPollGroup_Invalid;
    CSteamID lobby;
    DgsEvent queued[DGS_MAX_QUEUED];
    int queued_count = 0;
    SteamNetworkingMessage_t *held[DGS_MAX_MSGS];
    int held_count = 0;
    CCallResult<ShimState, LobbyCreated_t> lobby_created;

    STEAM_CALLBACK(ShimState, OnConnStatus, SteamNetConnectionStatusChangedCallback_t);
    STEAM_CALLBACK(ShimState, OnLobbyEnter, LobbyEnter_t);
    STEAM_CALLBACK(ShimState, OnJoinRequested, GameLobbyJoinRequested_t);
    STEAM_CALLBACK(ShimState, OnRichPresenceJoin, GameRichPresenceJoinRequested_t);

    void OnLobbyCreated(LobbyCreated_t *p, bool io_failure);

    void push(const DgsEvent &e) {
        if (queued_count < DGS_MAX_QUEUED) queued[queued_count++] = e;
    }
};

void ShimState::OnConnStatus(SteamNetConnectionStatusChangedCallback_t *p) {
    ISteamNetworkingSockets *s = SteamNetworkingSockets();
    DgsEvent e = {};
    e.conn = p->m_hConn;
    switch (p->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connecting:
        // Incoming connection on our listen socket: accept it.
        if (p->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid) {
            if (s->AcceptConnection(p->m_hConn) != k_EResultOK) s->CloseConnection(p->m_hConn, 0, "accept failed", false);
            else s->SetConnectionPollGroup(p->m_hConn, group);
        }
        break;
    case k_ESteamNetworkingConnectionState_Connected:
        s->SetConnectionPollGroup(p->m_hConn, group);
        e.type = DGS_EV_CONNECTED;
        push(e);
        break;
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        s->CloseConnection(p->m_hConn, 0, nullptr, false);
        e.type = DGS_EV_DISCONNECTED;
        push(e);
        break;
    default: break;
    }
}

void ShimState::OnLobbyCreated(LobbyCreated_t *p, bool io_failure) {
    if (io_failure || p->m_eResult != k_EResultOK) return;
    lobby = CSteamID(p->m_ulSteamIDLobby);
    char id[32];
    snprintf(id, sizeof(id), "%llu", (unsigned long long)SteamUser()->GetSteamID().ConvertToUint64());
    SteamMatchmaking()->SetLobbyData(lobby, "host_steamid", id);
    SteamMatchmaking()->SetLobbyData(lobby, "game", "deadground");
    char connect[64];
    snprintf(connect, sizeof(connect), "+connect_lobby %llu", (unsigned long long)p->m_ulSteamIDLobby);
    SteamFriends()->SetRichPresence("connect", connect);
    SteamFriends()->SetRichPresence("status", "Hosting a Deadground session");
    DgsEvent e = {};
    e.type = DGS_EV_LOBBY_CREATED;
    e.id = p->m_ulSteamIDLobby;
    push(e);
}

void ShimState::OnLobbyEnter(LobbyEnter_t *p) {
    if (p->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess) return;
    lobby = CSteamID(p->m_ulSteamIDLobby);
    DgsEvent e = {};
    e.type = DGS_EV_LOBBY_ENTERED;
    e.id = p->m_ulSteamIDLobby;
    e.owner = SteamMatchmaking()->GetLobbyOwner(lobby).ConvertToUint64();
    push(e);
}

void ShimState::OnJoinRequested(GameLobbyJoinRequested_t *p) {
    DgsEvent e = {};
    e.type = DGS_EV_JOIN_REQUESTED;
    e.id = p->m_steamIDLobby.ConvertToUint64();
    push(e);
}

void ShimState::OnRichPresenceJoin(GameRichPresenceJoinRequested_t *p) {
    // Connect string is "+connect_lobby <id>".
    const char *s = strstr(p->m_rgchConnect, "+connect_lobby ");
    if (!s) return;
    DgsEvent e = {};
    e.type = DGS_EV_JOIN_REQUESTED;
    e.id = strtoull(s + 15, nullptr, 10);
    push(e);
}

} // namespace

extern "C" {

bool dgs_init(void) {
    if (g_shim) return g_shim->ok;
    if (!SteamAPI_Init()) {
        fprintf(stderr, "steam: SteamAPI_Init failed (is Steam running? steam_appid.txt present?)\n");
        return false;
    }
    g_shim = new ShimState();
    g_shim->ok = true;
    SteamNetworkingUtils()->InitRelayNetworkAccess();
    g_shim->group = SteamNetworkingSockets()->CreatePollGroup();
    return true;
}

void dgs_shutdown(void) {
    if (!g_shim) return;
    for (int i = 0; i < g_shim->held_count; i++) g_shim->held[i]->Release();
    if (g_shim->listen != k_HSteamListenSocket_Invalid) SteamNetworkingSockets()->CloseListenSocket(g_shim->listen);
    SteamNetworkingSockets()->DestroyPollGroup(g_shim->group);
    dgs_leave_lobby();
    delete g_shim;
    g_shim = nullptr;
    SteamAPI_Shutdown();
}

bool dgs_available(void) { return g_shim && g_shim->ok; }
void dgs_run_callbacks(void) { if (g_shim) SteamAPI_RunCallbacks(); }
uint64_t dgs_my_id(void) { return g_shim ? SteamUser()->GetSteamID().ConvertToUint64() : 0; }
const char *dgs_persona_name(void) { return g_shim ? SteamFriends()->GetPersonaName() : ""; }

bool dgs_listen(void) {
    if (!g_shim) return false;
    if (g_shim->listen == k_HSteamListenSocket_Invalid) g_shim->listen = SteamNetworkingSockets()->CreateListenSocketP2P(0, 0, nullptr);
    return g_shim->listen != k_HSteamListenSocket_Invalid;
}

bool dgs_connect(uint64_t steam_id) {
    if (!g_shim) return false;
    SteamNetworkingIdentity id;
    id.SetSteamID64(steam_id);
    HSteamNetConnection c = SteamNetworkingSockets()->ConnectP2P(id, 0, 0, nullptr);
    if (c == k_HSteamNetConnection_Invalid) return false;
    SteamNetworkingSockets()->SetConnectionPollGroup(c, g_shim->group);
    return true;
}

bool dgs_send(uint32_t conn, const void *data, uint32_t size, bool reliable) {
    if (!g_shim) return false;
    int flags = reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_UnreliableNoNagle;
    return SteamNetworkingSockets()->SendMessageToConnection(conn, data, size, flags, nullptr) == k_EResultOK;
}

void dgs_close(uint32_t conn) {
    if (g_shim) SteamNetworkingSockets()->CloseConnection(conn, 0, "bye", true);
}

int dgs_poll(DgsEvent *out, int max) {
    if (!g_shim) return 0;
    for (int i = 0; i < g_shim->held_count; i++) g_shim->held[i]->Release();
    g_shim->held_count = 0;
    SteamAPI_RunCallbacks();
    int n = 0;
    for (int i = 0; i < g_shim->queued_count && n < max; i++) out[n++] = g_shim->queued[i];
    g_shim->queued_count = 0;
    SteamNetworkingMessage_t *msgs[64];
    int got = SteamNetworkingSockets()->ReceiveMessagesOnPollGroup(g_shim->group, msgs, 64);
    for (int i = 0; i < got; i++) {
        if (n >= max || g_shim->held_count >= DGS_MAX_MSGS) { msgs[i]->Release(); continue; }
        DgsEvent &e = out[n++];
        e = DgsEvent();
        e.type = DGS_EV_DATA;
        e.conn = msgs[i]->m_conn;
        e.data = msgs[i]->m_pData;
        e.size = (uint32_t)msgs[i]->m_cbSize;
        g_shim->held[g_shim->held_count++] = msgs[i];
    }
    return n;
}

bool dgs_stats(uint32_t conn, float *ping_ms, float *loss_pct) {
    if (!g_shim) return false;
    SteamNetConnectionRealTimeStatus_t st;
    if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(conn, &st, 0, nullptr) != k_EResultOK) return false;
    *ping_ms = (float)st.m_nPing;
    *loss_pct = (1.0f - st.m_flConnectionQualityLocal) * 100.0f;
    return true;
}

bool dgs_create_lobby(int max_members) {
    if (!g_shim) return false;
    SteamAPICall_t call = SteamMatchmaking()->CreateLobby(k_ELobbyTypeFriendsOnly, max_members);
    g_shim->lobby_created.Set(call, g_shim, &ShimState::OnLobbyCreated);
    return true;
}

bool dgs_join_lobby(uint64_t lobby) {
    if (!g_shim) return false;
    SteamMatchmaking()->JoinLobby(CSteamID(lobby));
    return true;
}

void dgs_leave_lobby(void) {
    if (!g_shim || !g_shim->lobby.IsValid()) return;
    SteamMatchmaking()->LeaveLobby(g_shim->lobby);
    g_shim->lobby = CSteamID();
    SteamFriends()->ClearRichPresence();
}

uint64_t dgs_current_lobby(void) { return g_shim && g_shim->lobby.IsValid() ? g_shim->lobby.ConvertToUint64() : 0; }

void dgs_invite_overlay(void) {
    if (g_shim && g_shim->lobby.IsValid()) SteamFriends()->ActivateGameOverlayInviteDialog(g_shim->lobby);
}

void dgs_set_rich_presence(const char *key, const char *value) {
    if (g_shim) SteamFriends()->SetRichPresence(key, value);
}

} // extern "C"
