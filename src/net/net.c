// TEMPORARY STUB - replaced by the real implementation.
#include "net.h"
#include "world/world.h"
#include "core/cvar.h"
bool net_init(void) { cvar_reg_int("net_port", 27015, 1, 65535, CVAR_ARCHIVE, "LAN port"); return true; }
void net_shutdown(void) {}
bool net_host(TransportKind kind, u16 port) { return false; }
bool net_connect(TransportKind kind, const char *address) { return false; }
void net_stop(void) {}
NetRole net_role(void) { return NET_OFF; }
bool net_steam_available(void) { return false; }
const char *net_transport_name(void) { return "none"; }
void net_update(float dt) {}
void net_host_after_tick(struct World *w) {}
void net_host_send_events(struct World *w) {}
void net_client_send_cmds(const UserCmd *h, int n, u32 s) {}
void net_client_interpolate(struct World *w, float dt) {}
u32 net_client_view_tick256(void) { return 0; }
void net_send_rpc(int type, int a, int b, int c) {}
NetStats net_stats(void) { NetStats s = {0}; s.transport = "none"; return s; }
void net_lan_refresh(void) {}
int net_lan_servers(const LanServer **out) { *out = 0; return 0; }
void net_steam_invite(void) {}
bool net_steam_in_lobby(void) { return false; }
