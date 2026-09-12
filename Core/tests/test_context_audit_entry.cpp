#include "test_framework.hpp"
#include "Core/Context/ContextAuditEntry.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(ContextAuditAction_ToString_CoversAllValues) {
    AISTUDIO_EXPECT(ToString(ContextAuditAction::Included) == "Included");
    AISTUDIO_EXPECT(ToString(ContextAuditAction::Excluded) == "Excluded");
    AISTUDIO_EXPECT(ToString(ContextAuditAction::Compressed) == "Compressed");
}
