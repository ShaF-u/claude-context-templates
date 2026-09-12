#include "test_framework.hpp"
#include "Core/Context/ContextSnapshot.hpp"

using namespace aistudio::core;

namespace {
ContextItem MakeItem(std::string id, std::int64_t tokens) {
    ContextItem item;
    item.id = std::move(id);
    item.estimated_tokens = tokens;
    return item;
}

ContextItem MakeItemWithSource(std::string id, ContextSourceKind source) {
    ContextItem item;
    item.id = std::move(id);
    item.source = source;
    return item;
}
} // namespace

AISTUDIO_TEST(MakeContextSnapshot_CapturesIncludedItemIds) {
    ContextSelection selection;
    selection.included = {MakeItem("a.cpp", 10), MakeItem("b.cpp", 20)};
    selection.excluded = {MakeItem("c.cpp", 999)};
    selection.used_tokens = 30;

    const auto snapshot = MakeContextSnapshot("snap-1", "Fix Player Attack", selection, 2000);

    AISTUDIO_EXPECT(snapshot.id == "snap-1");
    AISTUDIO_EXPECT(snapshot.task_name == "Fix Player Attack");
    AISTUDIO_EXPECT(snapshot.used_tokens == 30);
    AISTUDIO_EXPECT(snapshot.max_tokens == 2000);
    AISTUDIO_EXPECT(snapshot.included_item_ids.size() == 2);
    AISTUDIO_EXPECT(snapshot.included_item_ids[0] == "a.cpp");
    AISTUDIO_EXPECT(snapshot.included_item_ids[1] == "b.cpp");
}

AISTUDIO_TEST(MakeContextSnapshot_CapturesIncludedItemSourceKinds_ParallelToIds) {
    // docs/ROADMAP.md "Context Restore" -- ContextRestorer dispatches by
    // this recorded kind rather than guessing one from the id's shape.
    ContextSelection selection;
    selection.included = {
        MakeItemWithSource("a.cpp", ContextSourceKind::File),
        MakeItemWithSource("a.cpp:Foo", ContextSourceKind::Symbol),
        MakeItemWithSource("a.cpp->b.hpp", ContextSourceKind::Dependency),
        MakeItemWithSource("keyword:a.cpp:3", ContextSourceKind::Custom),
    };

    const auto snapshot = MakeContextSnapshot("snap-2", "task", selection, 1000);

    AISTUDIO_EXPECT(snapshot.included_item_source_kinds.size() == 4);
    AISTUDIO_EXPECT(snapshot.included_item_source_kinds[0] == ContextSourceKind::File);
    AISTUDIO_EXPECT(snapshot.included_item_source_kinds[1] == ContextSourceKind::Symbol);
    AISTUDIO_EXPECT(snapshot.included_item_source_kinds[2] == ContextSourceKind::Dependency);
    AISTUDIO_EXPECT(snapshot.included_item_source_kinds[3] == ContextSourceKind::Custom);
}

AISTUDIO_TEST(MakeContextSnapshot_StampsCreatedAt) {
    ContextSelection selection;
    const auto snapshot = MakeContextSnapshot("snap-1", "task", selection, 100);
    AISTUDIO_EXPECT(snapshot.created_at > 0);
}
