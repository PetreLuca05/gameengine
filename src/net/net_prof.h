// Netcode statistics for the profiler (docs/PERF_AUDIT.md "Netcode"): bytes per snapshot section
// and how often each section's bytes differ from the client's previous snapshot, entity delta fields
// (sent / bytes, and for grouped fields which members actually changed), message counts per type.
// Recorded only while a capture runs; written as the profile's "net" section.
#ifndef DG_NET_PROF_H
#define DG_NET_PROF_H

#include "core/base.h"

struct NetEnt;

typedef enum NetSection {
    NS_HEADER, NS_LOCAL, NS_SPECTATE, NS_TIME, NS_WEATHER, NS_VEHICLES, NS_ENTITIES,
    NS_COUNT
} NetSection;

void netprof_register(void);
void netprof_section(int client, NetSection s, const u8 *bytes, u32 len);
void netprof_entity(const struct NetEnt *base, const struct NetEnt *e, u32 mask, u32 bytes);
void netprof_entities_considered(int n);
void netprof_message(u8 type, u32 bytes, bool reliable);

#endif
