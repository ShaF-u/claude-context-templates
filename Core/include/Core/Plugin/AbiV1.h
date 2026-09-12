/* AbiV1.h -- the Plugin ABI, version 1 (docs/ROADMAP.md Phase 11 "Plugin
 * SDK" > "Commands/Queries/Capabilities" -- the DLL-boundary contract a
 * Plugin's native Backend implements).
 *
 * Deliberately plain C (no C++ features anywhere in this file): a Plugin
 * DLL calling into or being called from Core cannot safely pass
 * std::string/std::vector/std::any/vtables across a DLL boundary unless it
 * was built with the exact same compiler version, STL implementation, and
 * CRT linkage as this Core build -- a mismatch doesn't error, it corrupts
 * memory silently. This header is the ENTIRE public surface a Plugin
 * author needs: it has no dependency on any other Core header (Result.hpp,
 * Protocol.hpp, ...), so a Plugin project can implement AistudioPluginEntry
 * below without linking against or even seeing the rest of this codebase.
 *
 * String ownership: every AistudioPluginString a Plugin returns is a
 * BORROWED VIEW, valid only until the next call made on the same `self`
 * instance (the same convention POSIX strerror()/readdir() use for their
 * own reused-buffer returns) -- NOT until destroy(), and NOT owned by the
 * caller. A Plugin implementation keeps a per-instance scratch buffer it
 * overwrites each call; Core copies the bytes out (into a real
 * std::string) immediately after each call, before making another one on
 * that instance. This needs no release/free callback at all, which avoids
 * the classic cross-DLL heap hazard on Windows: memory allocated by one
 * DLL's CRT must never be freed by another's.
 *
 * Calls into a single AistudioPluginBackend instance are never concurrent
 * (Core serializes them, mirroring McpServer::Run/TaskQueue's own
 * single-threaded precedent elsewhere in this codebase) -- a Plugin
 * implementation does not need its own internal locking for that reason
 * alone.
 *
 * Versioning: AISTUDIO_PLUGIN_ABI_VERSION is a plain integer, bumped only
 * for a breaking change to this struct's shape (adding a field in the
 * middle, changing a signature -- appending a field at the END without
 * changing anything before it would NOT need a bump, though this v1 has no
 * such policy exercised yet). AistudioPluginEntry() takes the host's own
 * ABI version so a Plugin can also refuse to run against a Core it wasn't
 * built for (a bidirectional check, not just Core validating the Plugin).
 *
 * bind_host()/AistudioPluginHostServices, provide_context(), and
 * render_ui() (all added after v1's initial design, still version 1): no
 * real external Plugin exists yet -- only this repo's own test fixtures
 * -- so there is no binary compatibility to protect yet, and appending a
 * field to AistudioPluginBackendVTable costs nothing today. Once a real
 * Plugin ecosystem depends on this ABI, ANY further field addition --
 * even an append -- must bump AISTUDIO_PLUGIN_ABI_VERSION instead of
 * editing structs in place, because an old Plugin binary's vtable may
 * simply be too short for Core to read a newly-appended field from
 * safely.
 */
#ifndef AISTUDIO_CORE_PLUGIN_ABI_V1_H
#define AISTUDIO_CORE_PLUGIN_ABI_V1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AISTUDIO_PLUGIN_ABI_VERSION 1u

#if defined(_WIN32)
#define AISTUDIO_PLUGIN_EXPORT __declspec(dllexport)
#else
#define AISTUDIO_PLUGIN_EXPORT
#endif

/* A borrowed, non-owning view into Plugin-owned memory -- see the file
 * comment above for its lifetime rule. Not necessarily NUL-terminated;
 * always use `len`. */
typedef struct AistudioPluginString {
    const char* data;
    size_t len;
} AistudioPluginString;

/* Opaque instance handle -- a Plugin defines what this actually points to
 * (its own struct); Core never dereferences it, only passes it back
 * through the vtable functions below. */
typedef struct AistudioPluginBackend AistudioPluginBackend;

/* Mirrors Core::BackendHealth (Core/include/Core/Backend/IBackend.hpp) as
 * a plain int for the ABI boundary -- Core's own C++ enum is not part of
 * this frozen contract and could reorder its values over time, so the
 * adapter on Core's side maps this explicitly by name, never by casting
 * the int directly to BackendHealth. */
typedef enum AistudioPluginHealth {
    AISTUDIO_PLUGIN_HEALTH_UNKNOWN = 0,
    AISTUDIO_PLUGIN_HEALTH_HEALTHY = 1,
    AISTUDIO_PLUGIN_HEALTH_DEGRADED = 2,
    AISTUDIO_PLUGIN_HEALTH_UNAVAILABLE = 3
} AistudioPluginHealth;

/* Services Core offers TO a Plugin (the reverse direction from every
 * other function pointer in this file, which the Plugin implements for
 * Core to call) -- see AistudioPluginBackendVTable::bind_host below.
 *
 * publish_event(): publishes an Event onto Core's EventBus, attributed to
 * this Plugin Backend (Core sets Event::source to this Backend's own
 * Id() -- a Plugin never needs to specify its own identity, only
 * `event_name` and an optional `payload_json`). MUST be called only
 * synchronously, from within a call Core itself made into this same
 * AistudioPluginBackend instance (start/stop/configure/dispatch/handle)
 * -- never from a Plugin-owned background thread. EventBus's own
 * dispatch is synchronous (a subscriber's handler runs on whatever
 * thread called Publish()), so calling from an unexpected thread would
 * run Core's own subscriber callbacks there too, which this contract
 * avoids by keeping Core always the one driving execution, the same
 * threading model as every other AbiV1.h call.
 *
 * `payload_json` is borrowed for the duration of this call only (Core
 * copies it out before returning, the reverse-direction counterpart of
 * how the Plugin must treat command_json/query_json above) -- pass a
 * zero-length AistudioPluginString for no payload. Malformed JSON is
 * logged and the publish is silently skipped rather than surfaced as an
 * error -- this is a fire-and-forget notification, matching EventBus::
 * Publish() itself having no error return even for Core's own internal
 * callers. */
typedef struct AistudioPluginHostServices {
    void* host_context; /* opaque, Core-owned; passed back verbatim */
    void (*publish_event)(void* host_context, AistudioPluginString event_name, AistudioPluginString payload_json);
} AistudioPluginHostServices;

/* The full contract a Plugin Backend implements, one instance per
 * create()'d AistudioPluginBackend*. Every function pointer here must be
 * non-NULL (Core rejects a vtable with any NULL entry rather than crash
 * on first use) except where noted.
 *
 * Every fallible operation (start/stop/configure/dispatch/handle) shares
 * the same shape: return 0 for Ok, nonzero for Err, and fill
 * `*out_payload_or_error` with the JSON result payload (Ok) or a
 * human-readable error message (Err) -- both cases use the same borrowed-
 * view lifetime rule as every other AistudioPluginString here. */
typedef struct AistudioPluginBackendVTable {
    /* Must equal AISTUDIO_PLUGIN_ABI_VERSION at the time this vtable was
     * built -- Core checks this before touching any function pointer
     * below. */
    uint32_t abi_version;

    AistudioPluginBackend* (*create)(void);
    void (*destroy)(AistudioPluginBackend* self);

    /* OPTIONAL (may be NULL, unlike every other function pointer here) --
     * a Plugin that never publishes Events has nothing to do with this.
     * Called by Core exactly once, immediately after create() succeeds
     * and before any other call. `host` points to storage Core owns for
     * this AistudioPluginBackend instance's entire lifetime -- the
     * Plugin may retain the pointer itself or copy the small struct's
     * contents, either is fine, but must not use it after destroy(). */
    void (*bind_host)(AistudioPluginBackend* self, const AistudioPluginHostServices* host);

    AistudioPluginString (*id)(const AistudioPluginBackend* self);
    AistudioPluginString (*name)(const AistudioPluginBackend* self);
    AistudioPluginString (*version)(const AistudioPluginBackend* self);

    /* Capability names -- mirrors IBackend::Capabilities(), exposed as
     * count + indexed accessor rather than an array the caller would need
     * a matching free function to release. */
    size_t (*capability_count)(const AistudioPluginBackend* self);
    AistudioPluginString (*capability_at)(const AistudioPluginBackend* self, size_t index);

    /* Returns an AistudioPluginHealth value. */
    int (*health)(const AistudioPluginBackend* self);

    int (*start)(AistudioPluginBackend* self, AistudioPluginString* out_error);
    int (*stop)(AistudioPluginBackend* self, AistudioPluginString* out_error);
    /* config_json: a JSON object Core serialized from whatever Config it
     * has for this Plugin (conventionally under `backend.<Id()>.*`, same
     * as IBackend::Configure's own convention) -- borrowed for the
     * duration of this call only, not retained. */
    int (*configure)(AistudioPluginBackend* self, AistudioPluginString config_json, AistudioPluginString* out_error);

    /* command_json / query_json: `{"name":"...","request_id":"...",
     * "correlation_id":"...","payload":"..."}` -- matches the same
     * string-only payload convention Core/API/ApiServer.cpp's own
     * POST /api/backends/{id}/commands endpoint already uses for its
     * Command::payload, kept consistent rather than inventing a richer
     * shape only Plugins would see. */
    int (*dispatch)(AistudioPluginBackend* self, AistudioPluginString command_json,
                     AistudioPluginString* out_payload_or_error);
    int (*handle)(AistudioPluginBackend* self, AistudioPluginString query_json,
                  AistudioPluginString* out_payload_or_error);

    /* OPTIONAL (may be NULL) -- Context Provider System (docs/MASTER_SPEC.md
     * #71). intent_json: `{"intent":"free text"}`. On success (0),
     * *out_items_or_error is a JSON array of item objects, each at
     * minimum `{"id":"...","content":"..."}` with an optional integer
     * "priority" (0-100, default 0 if absent -- see docs/MASTER_SPEC.md
     * #14's scale, same one Core/Context/ContextItem.hpp's own priority
     * field documents). Core always recomputes the token estimate from
     * `content` itself rather than trusting a Plugin-supplied count, so
     * there is no "estimated_tokens" field to set here. On failure
     * (nonzero), *out_items_or_error is a human-readable error message,
     * same convention as dispatch/handle. */
    int (*provide_context)(AistudioPluginBackend* self, AistudioPluginString intent_json,
                            AistudioPluginString* out_items_or_error);

    /* OPTIONAL (may be NULL) -- UI Extension System (docs/ROADMAP.md
     * Phase 11 "UI extensions"). No input; called by the GUI process
     * on demand (its own "refresh" action, not every frame -- see
     * Core/Backend/UiDescription.hpp) rather than by aistudio_core_cli.
     * On success (0), *out_ui_json_or_error is
     * `{"title":"...","elements":[...]}` where each element is
     * `{"kind":"text","text":"..."}`, `{"kind":"separator"}`, or
     * `{"kind":"button","text":"label","action_id":"..."}` -- a Button's
     * action_id becomes Command::name in the Dispatch() call the GUI
     * makes when a user clicks it, so a Plugin that already implements
     * dispatch needs nothing new to handle a button. On failure
     * (nonzero), *out_ui_json_or_error is a human-readable error
     * message, same convention as dispatch/handle/provide_context. */
    int (*render_ui)(AistudioPluginBackend* self, AistudioPluginString* out_ui_json_or_error);
} AistudioPluginBackendVTable;

/* The one symbol every Plugin DLL exports, under exactly this name
 * (Core resolves it via PluginLoader::ResolveSymbol(id,
 * "AistudioPluginEntry")). `host_abi_version` is always
 * AISTUDIO_PLUGIN_ABI_VERSION from Core's own build; a Plugin that can't
 * support it should return NULL rather than a vtable it can't honor. */
typedef const AistudioPluginBackendVTable* (*AistudioPluginEntryFn)(uint32_t host_abi_version);

#ifdef __cplusplus
}
#endif

#endif /* AISTUDIO_CORE_PLUGIN_ABI_V1_H */
