// Grid inventory wire format (docs/INVENTORY.md "Networking"): a tree as its live instances, an
// intent as 18 bytes. Little-endian, field by field (no struct layout on the wire). Pure.
#ifndef DG_INV_WIRE_H
#define DG_INV_WIRE_H

#include "inv_intent.h"

#define INV_WIRE_ITEM 21
#define INV_WIRE_TREE_MAX (5 + INV_MAX_ITEMS * INV_WIRE_ITEM)
#define INV_WIRE_INTENT 18

int inv_wire_write_tree(const InvTree *t, u8 *out, int max);         // bytes written, 0 if it doesn't fit
int inv_wire_read_tree(InvTree *t, const u8 *in, int size);          // bytes read, 0 on a malformed tree
void inv_wire_write_intent(const InvIntent *in, u8 out[INV_WIRE_INTENT]);
void inv_wire_read_intent(InvIntent *in, const u8 b[INV_WIRE_INTENT]);

#endif
