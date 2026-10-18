// Directed LAN broadcast addresses (docs/PLATFORM_MACOS.md "LAN"): macOS sends 255.255.255.255 out of
// the primary interface only, so the discovery query also goes to each up, broadcast-capable IPv4
// interface's subnet broadcast (Ethernet + Wi-Fi + a VM bridge). Windows routes the limited broadcast
// to every interface already and returns 0 here.
#ifndef DG_LAN_IFADDR_H
#define DG_LAN_IFADDR_H

#include "core/base.h"

// Host-order IPv4 broadcast addresses, at most max; returns the count.
int lan_broadcast_addrs(u32 *out, int max);

#endif
