// Steamworks shim (C++). Keeps all Steam C++ types out of the C code base.
// Written against Steamworks SDK 1.5x/1.6x (ISteamNetworkingSockets + ISteamMatchmaking).
#include "steam_shim.h"
#include "steam/steam_api.h"
#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"
#include "steam/isteammatchmaking.h"
#include "steam/isteamfriends.h"
#include "steam/isteamutils.h"
#include <stdio.h>
#include <stdlib.h>
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
    CCallResult<ShimState, LobbyMatchList_t> lobby_list;
    int list_status = 0;          // 0 idle, 1 pending, 2 ready
    int list_count = 0;
    CSteamID list_ids[64];

    STEAM_CALLBACK(ShimState, OnConnStatus, SteamNetConnectionStatusChangedCallback_t);
    STEAM_CALLBACK(ShimState, OnLobbyEnter, LobbyEnter_t);
    STEAM_CALLBACK(ShimState, OnJoinRequested, GameLobbyJoinRequested_t);
    STEAM_CALLBACK(ShimState, OnRichPresenceJoin, GameRichPresenceJoinRequested_t);

    void OnLobbyCreated(LobbyCreated_t *p, bool io_failure);
    void OnLobbyList(LobbyMatchList_t *p, bool io_failure);

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

void ShimState::OnLobbyList(LobbyMatchList_t *p, bool io_failure) {
    list_count = 0;
    if (!io_failure) {
        uint32 n = p->m_nLobbiesMatching;
        for (uint32 i = 0; i < n && list_count < 64; i++) list_ids[list_count++] = SteamMatchmaking()->GetLobbyByIndex((int)i);
    }
    list_status = 2;
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

bool dgs_create_lobby_typed(int max_members, int type) {
    if (!g_shim) return false;
    ELobbyType t = type == DGS_LOBBY_PUBLIC ? k_ELobbyTypePublic : (type == DGS_LOBBY_PRIVATE ? k_ELobbyTypePrivate : k_ELobbyTypeFriendsOnly);
    SteamAPICall_t call = SteamMatchmaking()->CreateLobby(t, max_members);
    g_shim->lobby_created.Set(call, g_shim, &ShimState::OnLobbyCreated);
    return true;
}

void dgs_set_lobby_data(const char *key, const char *value) {
    if (g_shim && g_shim->lobby.IsValid()) SteamMatchmaking()->SetLobbyData(g_shim->lobby, key, value);
}

void dgs_set_lobby_type(int type) {
    if (!g_shim || !g_shim->lobby.IsValid()) return;
    ELobbyType t = type == DGS_LOBBY_PUBLIC ? k_ELobbyTypePublic : (type == DGS_LOBBY_PRIVATE ? k_ELobbyTypePrivate : k_ELobbyTypeFriendsOnly);
    SteamMatchmaking()->SetLobbyType(g_shim->lobby, t);
}

void dgs_set_lobby_joinable(bool joinable) {
    if (g_shim && g_shim->lobby.IsValid()) SteamMatchmaking()->SetLobbyJoinable(g_shim->lobby, joinable);
}

uint64_t dgs_lobby_owner(void) {
    if (!g_shim || !g_shim->lobby.IsValid()) return 0;
    return SteamMatchmaking()->GetLobbyOwner(g_shim->lobby).ConvertToUint64();
}

bool dgs_set_lobby_owner(uint64_t steam_id) {
    if (!g_shim || !g_shim->lobby.IsValid()) return false;
    return SteamMatchmaking()->SetLobbyOwner(g_shim->lobby, CSteamID(steam_id));
}

bool dgs_conn_steam_id(uint32_t conn, uint64_t *out) {
    if (!g_shim) return false;
    SteamNetConnectionInfo_t info;
    if (!SteamNetworkingSockets()->GetConnectionInfo(conn, &info)) return false;
    *out = info.m_identityRemote.GetSteamID64();
    return *out != 0;
}

float dgs_relay_ping_ms(void) {
    if (!g_shim) return -1.0f;
    SteamNetworkingPOPID pops[64];
    int n = SteamNetworkingUtils()->GetPOPList(pops, 64);
    int best = -1;
    for (int i = 0; i < n; i++) {
        SteamNetworkingPOPID via = 0;
        int ms = SteamNetworkingUtils()->GetPingToDataCenter(pops[i], &via);
        if (ms >= 0 && (best < 0 || ms < best)) best = ms;
    }
    return best < 0 ? -1.0f : (float)best;
}

void dgs_request_lobby_list(const char *map, int distance, int min_free_slots) {
    if (!g_shim) return;
    ISteamMatchmaking *mm = SteamMatchmaking();
    mm->AddRequestLobbyListStringFilter("game", "deadground", k_ELobbyComparisonEqual);
    if (map && map[0]) mm->AddRequestLobbyListStringFilter("map", map, k_ELobbyComparisonEqual);
    if (min_free_slots > 0) mm->AddRequestLobbyListFilterSlotsAvailable(min_free_slots);
    mm->AddRequestLobbyListDistanceFilter((ELobbyDistanceFilter)(distance < 0 ? 1 : (distance > 3 ? 3 : distance)));
    mm->AddRequestLobbyListResultCountFilter(64);
    SteamAPICall_t call = mm->RequestLobbyList();
    g_shim->lobby_list.Set(call, g_shim, &ShimState::OnLobbyList);
    g_shim->list_status = 1;
}

int dgs_lobby_list_status(void) { return g_shim ? g_shim->list_status : 0; }

static void lobby_str(CSteamID lob, const char *key, char *out, size_t n) {
    const char *v = SteamMatchmaking()->GetLobbyData(lob, key);
    snprintf(out, n, "%s", v ? v : "");
}

int dgs_lobby_list(DgsLobbyInfo *out, int max) {
    if (!g_shim) return 0;
    int n = 0;
    for (int i = 0; i < g_shim->list_count && n < max; i++) {
        CSteamID lob = g_shim->list_ids[i];
        DgsLobbyInfo &d = out[n++];
        memset(&d, 0, sizeof(d));
        d.id = lob.ConvertToUint64();
        CSteamID owner = SteamMatchmaking()->GetLobbyOwner(lob);
        d.owner = owner.ConvertToUint64();
        lobby_str(lob, "name", d.name, sizeof(d.name));
        lobby_str(lob, "map", d.map, sizeof(d.map));
        lobby_str(lob, "version", d.version, sizeof(d.version));
        lobby_str(lob, "state", d.state, sizeof(d.state));
        char tmp[16];
        lobby_str(lob, "players", tmp, sizeof(tmp)); d.players = atoi(tmp);
        lobby_str(lob, "max", tmp, sizeof(tmp)); d.max_players = atoi(tmp);
        lobby_str(lob, "privacy", tmp, sizeof(tmp)); d.privacy = atoi(tmp);
        lobby_str(lob, "season", tmp, sizeof(tmp)); d.season = atoi(tmp);
        d.members = SteamMatchmaking()->GetNumLobbyMembers(lob);
        d.friend_owner = owner.IsValid() && SteamFriends()->GetFriendRelationship(owner) == k_EFriendRelationshipFriend;
    }
    return n;
}

int dgs_friends(DgsFriend *out, int max) {
    if (!g_shim) return 0;
    int count = SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
    int n = 0;
    AppId_t app = SteamUtils()->GetAppID();
    for (int i = 0; i < count && n < max; i++) {
        CSteamID f = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
        DgsFriend &d = out[n++];
        memset(&d, 0, sizeof(d));
        d.id = f.ConvertToUint64();
        snprintf(d.name, sizeof(d.name), "%s", SteamFriends()->GetFriendPersonaName(f));
        EPersonaState st = SteamFriends()->GetFriendPersonaState(f);
        d.state = st == k_EPersonaStateOffline ? 0 : (st == k_EPersonaStateOnline || st == k_EPersonaStateLookingToPlay || st == k_EPersonaStateLookingToTrade ? 1 : 2);
        FriendGameInfo_t gi;
        if (SteamFriends()->GetFriendGamePlayed(f, &gi) && gi.m_gameID.AppID() == app) {
            d.in_this_game = true;
            d.lobby = gi.m_steamIDLobby.IsValid() ? gi.m_steamIDLobby.ConvertToUint64() : 0;
        }
    }
    return n;
}

bool dgs_invite_friend(uint64_t steam_id) {
    if (!g_shim || !g_shim->lobby.IsValid()) return false;
    return SteamMatchmaking()->InviteUserToLobby(g_shim->lobby, CSteamID(steam_id));
}

bool dgs_avatar_rgba(uint64_t steam_id, int *w, int *h, uint8_t *out, int cap) {
    if (!g_shim) return false;
    int img = SteamFriends()->GetSmallFriendAvatar(CSteamID(steam_id));
    if (img <= 0) return false;   // 0: none, -1: still loading
    uint32 iw = 0, ih = 0;
    if (!SteamUtils()->GetImageSize(img, &iw, &ih) || (int)(iw * ih * 4) > cap) return false;
    if (!SteamUtils()->GetImageRGBA(img, out, (int)(iw * ih * 4))) return false;
    *w = (int)iw;
    *h = (int)ih;
    return true;
}

} // extern "C"
