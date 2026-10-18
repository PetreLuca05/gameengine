#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE   // getifaddrs under -std=c11 (glibc)
#endif
#include "lan_ifaddr.h"

#if defined(_WIN32)
int lan_broadcast_addrs(u32 *out, int max) {
    (void)out;
    (void)max;
    return 0;
}
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

int lan_broadcast_addrs(u32 *out, int max) {
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) return 0;
    int n = 0;
    for (struct ifaddrs *i = list; i && n < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_broadaddr) continue;
        if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_BROADCAST) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        u32 b = ntohl(((const struct sockaddr_in *)i->ifa_broadaddr)->sin_addr.s_addr);
        if (b == 0 || b == 0xFFFFFFFFu) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) dup |= out[k] == b;
        if (!dup) out[n++] = b;
    }
    freeifaddrs(list);
    return n;
}
#endif
