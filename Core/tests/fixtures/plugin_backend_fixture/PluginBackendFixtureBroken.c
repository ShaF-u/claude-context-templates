/* PluginBackendFixtureBroken.c -- a second, deliberately-broken fixture
 * DLL for test_plugin_backend_adapter.cpp: its vtable has a NULL `create`
 * pointer, proving LoadPluginBackend()'s NULL-function-pointer check
 * actually rejects a real (if malformed) plugin DLL rather than only a
 * hand-built in-memory struct the test process constructed itself.
 */
#include "Core/Plugin/AbiV1.h"

#include <stddef.h>

static void Destroy(AistudioPluginBackend* self) { (void)self; }
static AistudioPluginString Id(const AistudioPluginBackend* self) {
    (void)self;
    AistudioPluginString s = {"broken", 6};
    return s;
}

/* Designated initializers (C11), not positional -- every field this
 * struct doesn't name is implicitly NULL, which is exactly what this
 * fixture wants (everything except destroy/id is deliberately absent),
 * and stays correct automatically if AbiV1.h's struct grows again. */
static const AistudioPluginBackendVTable kVTable = {
    .abi_version = AISTUDIO_PLUGIN_ABI_VERSION,
    .create = NULL, /* deliberately NULL -- this is the point of this fixture */
    .destroy = Destroy,
    .id = Id,
};

AISTUDIO_PLUGIN_EXPORT const AistudioPluginBackendVTable* AistudioPluginEntry(uint32_t host_abi_version) {
    if (host_abi_version != AISTUDIO_PLUGIN_ABI_VERSION) {
        return NULL;
    }
    return &kVTable;
}
