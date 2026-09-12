/* PluginBackendFixture.c -- test-only fixture Plugin DLL for
 * test_plugin_backend_adapter.cpp. Deliberately plain C (compiled as C,
 * not C++, via this directory's CMakeLists.txt) to prove
 * PluginBackendAdapter actually crosses a real DLL boundary using
 * AbiV1.h's C ABI, rather than only exercising in-process C++ against a
 * fake vtable. Not part of aistudio_core or aistudio_core_cli -- built
 * only for aistudio_core_tests to load via PluginLoader.
 *
 * Behavior: id/name/version/capabilities are fixed strings; health is
 * always Healthy; start/stop always succeed; configure always succeeds
 * and records the JSON it received; dispatch/handle echo the command/
 * query JSON they were given back inside {"echo": <input>}, UNLESS the
 * input contains the literal substring "fail", in which case they return
 * an error instead -- gives the test suite a way to exercise both the Ok
 * and Err paths through the real ABI boundary without needing a richer
 * fixture protocol.
 */
#include "Core/Plugin/AbiV1.h"

#include <stdlib.h>
#include <string.h>

struct AistudioPluginBackend {
    /* Scratch buffer AbiV1.h's borrowed-view convention requires: each
     * call that returns an AistudioPluginString overwrites this and
     * returns a view into it, valid only until the next call on this
     * same instance. */
    char scratch[512];
    char config_json[256];
    /* Set once by BindHost(), NULL until then. */
    const AistudioPluginHostServices* host;
};

static AistudioPluginString FromLiteral(const char* text) {
    AistudioPluginString s;
    s.data = text;
    s.len = strlen(text);
    return s;
}

static AistudioPluginString FromScratch(AistudioPluginBackend* self, const char* text) {
    const size_t n = strlen(text);
    const size_t copy_len = n < sizeof(self->scratch) - 1 ? n : sizeof(self->scratch) - 1;
    memcpy(self->scratch, text, copy_len);
    self->scratch[copy_len] = '\0';
    AistudioPluginString s;
    s.data = self->scratch;
    s.len = copy_len;
    return s;
}

static AistudioPluginBackend* Create(void) {
    AistudioPluginBackend* self = (AistudioPluginBackend*)calloc(1, sizeof(AistudioPluginBackend));
    return self;
}

static void Destroy(AistudioPluginBackend* self) { free(self); }

static void BindHost(AistudioPluginBackend* self, const AistudioPluginHostServices* host) { self->host = host; }

static AistudioPluginString Id(const AistudioPluginBackend* self) {
    (void)self;
    return FromLiteral("fixture");
}

static AistudioPluginString Name(const AistudioPluginBackend* self) {
    (void)self;
    return FromLiteral("Fixture Plugin Backend");
}

static AistudioPluginString Version(const AistudioPluginBackend* self) {
    (void)self;
    return FromLiteral("1.0.0");
}

static size_t CapabilityCount(const AistudioPluginBackend* self) {
    (void)self;
    return 2;
}

static AistudioPluginString CapabilityAt(const AistudioPluginBackend* self, size_t index) {
    (void)self;
    if (index == 0) {
        return FromLiteral("fixture.ping");
    }
    return FromLiteral("fixture.echo");
}

static int Health(const AistudioPluginBackend* self) {
    (void)self;
    return AISTUDIO_PLUGIN_HEALTH_HEALTHY;
}

static int Start(AistudioPluginBackend* self, AistudioPluginString* out_error) {
    (void)self;
    (void)out_error;
    return 0;
}

static int Stop(AistudioPluginBackend* self, AistudioPluginString* out_error) {
    (void)self;
    (void)out_error;
    return 0;
}

static int Configure(AistudioPluginBackend* self, AistudioPluginString config_json, AistudioPluginString* out_error) {
    (void)out_error;
    const size_t n = config_json.len < sizeof(self->config_json) - 1 ? config_json.len : sizeof(self->config_json) - 1;
    memcpy(self->config_json, config_json.data, n);
    self->config_json[n] = '\0';
    return 0;
}

static int ContainsSubstring(AistudioPluginString s, const char* needle, size_t needle_len) {
    /* Deliberately not relying on NUL-termination of `s` (AbiV1.h's own
     * contract says not to assume it) -- scan the raw bytes instead of
     * calling strstr() on s.data directly. */
    if (s.len < needle_len) {
        return 0;
    }
    for (size_t i = 0; i + needle_len <= s.len; ++i) {
        if (memcmp(s.data + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int EchoOrFail(AistudioPluginBackend* self, AistudioPluginString input, AistudioPluginString* out) {
    if (ContainsSubstring(input, "fail", 4)) {
        *out = FromScratch(self, "fixture reports failure");
        return 1;
    }
    /* Exercises AistudioPluginHostServices::publish_event -- the actual
     * Plugin-calls-Core direction of the ABI, not just Core-calls-Plugin
     * -- when self->host was set by BindHost() (i.e. the caller's vtable
     * had bind_host wired up). A no-op if host is NULL (either bind_host
     * itself was never called by this caller, or this build's vtable
     * doesn't set it) so this fixture stays usable for tests that don't
     * care about Events at all. */
    if (self->host != NULL && self->host->publish_event != NULL &&
        ContainsSubstring(input, "emit_event", 10)) {
        AistudioPluginString event_name = FromLiteral("FixtureEvent");
        AistudioPluginString payload = FromLiteral("{\"triggered\":true}");
        self->host->publish_event(self->host->host_context, event_name, payload);
    }
    /* Minimal hand-built JSON wrapping -- input.data isn't NUL-terminated
     * so it's copied by length, not treated as a C string. */
    char buffer[512];
    const char prefix[] = "{\"echo\":";
    const char suffix[] = "}";
    size_t pos = 0;
    memcpy(buffer + pos, prefix, sizeof(prefix) - 1);
    pos += sizeof(prefix) - 1;
    const size_t input_len = input.len < sizeof(buffer) - pos - sizeof(suffix) ? input.len
                                                                                : sizeof(buffer) - pos - sizeof(suffix);
    memcpy(buffer + pos, input.data, input_len);
    pos += input_len;
    memcpy(buffer + pos, suffix, sizeof(suffix) - 1);
    pos += sizeof(suffix) - 1;
    buffer[pos] = '\0';

    const size_t n = pos < sizeof(self->scratch) - 1 ? pos : sizeof(self->scratch) - 1;
    memcpy(self->scratch, buffer, n);
    self->scratch[n] = '\0';
    out->data = self->scratch;
    out->len = n;
    return 0;
}

static int Dispatch(AistudioPluginBackend* self, AistudioPluginString command_json,
                     AistudioPluginString* out_payload_or_error) {
    return EchoOrFail(self, command_json, out_payload_or_error);
}

/* Exercises AistudioPluginBackendVTable::provide_context (Context
 * Provider System, docs/MASTER_SPEC.md #71) -- three fixed behaviors
 * selected by substring, mirroring the "fail"/"emit_event" trigger
 * convention already used above: "fixture_context" returns one real
 * item; "fixture_secret" returns one item whose id looks like a
 * Sandbox-denied path (for testing that ContextRetriever's firewall
 * check actually applies to Backend-provided items); anything else
 * returns an empty array (still valid JSON, zero items). */
static int ProvideContext(AistudioPluginBackend* self, AistudioPluginString intent_json,
                           AistudioPluginString* out_items_or_error) {
    if (ContainsSubstring(intent_json, "fixture_context", 15)) {
        *out_items_or_error =
            FromScratch(self, "[{\"id\":\"fixture:item1\",\"content\":\"fixture context item\",\"priority\":42}]");
        return 0;
    }
    if (ContainsSubstring(intent_json, "fixture_secret", 14)) {
        *out_items_or_error =
            FromScratch(self, "[{\"id\":\"my_secret.txt\",\"content\":\"should be firewalled\",\"priority\":10}]");
        return 0;
    }
    *out_items_or_error = FromScratch(self, "[]");
    return 0;
}

static int Handle(AistudioPluginBackend* self, AistudioPluginString query_json,
                   AistudioPluginString* out_payload_or_error) {
    return EchoOrFail(self, query_json, out_payload_or_error);
}

/* Designated initializers (C11) rather than positional -- this struct has
 * grown once already (bind_host) since this fixture was first written;
 * positional init would have silently shifted every field after the
 * insertion point instead of failing to compile. */
/* Exercises AistudioPluginBackendVTable::render_ui (UI Extension System,
 * docs/ROADMAP.md Phase 11) -- a fixed panel: one text line, a
 * separator, and one button whose action_id ("fixture_button_clicked")
 * is a normal Dispatch()-able command name (falls through to
 * EchoOrFail()'s usual echo behavior, same as any other command). */
static int RenderUi(AistudioPluginBackend* self, AistudioPluginString* out_ui_or_error) {
    *out_ui_or_error = FromScratch(self,
                                    "{\"title\":\"Fixture Panel\",\"elements\":["
                                    "{\"kind\":\"text\",\"text\":\"hello from fixture\"},"
                                    "{\"kind\":\"separator\"},"
                                    "{\"kind\":\"button\",\"text\":\"Ping\",\"action_id\":\"fixture_button_clicked\"}"
                                    "]}");
    return 0;
}

static const AistudioPluginBackendVTable kVTable = {
    .abi_version = AISTUDIO_PLUGIN_ABI_VERSION,
    .create = Create,
    .destroy = Destroy,
    .bind_host = BindHost,
    .id = Id,
    .name = Name,
    .version = Version,
    .capability_count = CapabilityCount,
    .capability_at = CapabilityAt,
    .health = Health,
    .start = Start,
    .stop = Stop,
    .configure = Configure,
    .dispatch = Dispatch,
    .handle = Handle,
    .provide_context = ProvideContext,
    .render_ui = RenderUi,
};

AISTUDIO_PLUGIN_EXPORT const AistudioPluginBackendVTable* AistudioPluginEntry(uint32_t host_abi_version) {
    if (host_abi_version != AISTUDIO_PLUGIN_ABI_VERSION) {
        return NULL;
    }
    return &kVTable;
}
