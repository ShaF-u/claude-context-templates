#include "test_framework.hpp"
#include "Core/Util/Glob.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(GlobMatch_ExactMatch) {
    AISTUDIO_EXPECT(GlobMatch("file.cpp", "file.cpp"));
}

AISTUDIO_TEST(GlobMatch_Wildcard_MatchesSuffix) {
    AISTUDIO_EXPECT(GlobMatch("player.delete", "*.delete"));
    AISTUDIO_EXPECT(!GlobMatch("player.deleteX", "*.delete"));
}

AISTUDIO_TEST(GlobMatch_Wildcard_MatchesPrefix) {
    AISTUDIO_EXPECT(GlobMatch("security.audit", "security.*"));
}

AISTUDIO_TEST(GlobMatch_NoMatch) {
    AISTUDIO_EXPECT(!GlobMatch("file.cpp", "file.h"));
}

AISTUDIO_TEST(GlobMatch_DotIsLiteral_NotAnyCharacter) {
    // '.' in the pattern must match a literal '.', not "any character"
    // like a naive translation to regex could allow.
    AISTUDIO_EXPECT(!GlobMatch("fileXcpp", "file.cpp"));
}
