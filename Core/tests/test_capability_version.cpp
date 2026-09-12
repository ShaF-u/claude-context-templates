#include "test_framework.hpp"
#include "Core/Backend/CapabilityVersion.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(CapabilityVersion_Parse_FullVersion) {
    const auto version = CapabilityVersion::Parse("2.1.3");
    AISTUDIO_EXPECT(version.has_value());
    AISTUDIO_EXPECT(version->major == 2);
    AISTUDIO_EXPECT(version->minor == 1);
    AISTUDIO_EXPECT(version->patch == 3);
}

AISTUDIO_TEST(CapabilityVersion_Parse_MissingPartsDefaultToZero) {
    const auto major_only = CapabilityVersion::Parse("2");
    AISTUDIO_EXPECT(major_only.has_value());
    AISTUDIO_EXPECT(major_only->minor == 0);
    AISTUDIO_EXPECT(major_only->patch == 0);

    const auto major_minor = CapabilityVersion::Parse("2.5");
    AISTUDIO_EXPECT(major_minor.has_value());
    AISTUDIO_EXPECT(major_minor->minor == 5);
    AISTUDIO_EXPECT(major_minor->patch == 0);
}

AISTUDIO_TEST(CapabilityVersion_Parse_Empty_ReturnsNullopt) {
    AISTUDIO_EXPECT(!CapabilityVersion::Parse("").has_value());
}

AISTUDIO_TEST(CapabilityVersion_Parse_NonNumeric_ReturnsNullopt) {
    AISTUDIO_EXPECT(!CapabilityVersion::Parse("a.b.c").has_value());
}

AISTUDIO_TEST(CapabilityVersion_Parse_TooManyParts_ReturnsNullopt) {
    AISTUDIO_EXPECT(!CapabilityVersion::Parse("1.2.3.4").has_value());
}

AISTUDIO_TEST(CapabilityVersion_Parse_NegativeNumber_ReturnsNullopt) {
    AISTUDIO_EXPECT(!CapabilityVersion::Parse("-1.0.0").has_value());
}

AISTUDIO_TEST(CapabilityVersion_Equality) {
    AISTUDIO_EXPECT((CapabilityVersion{1, 2, 3} == CapabilityVersion{1, 2, 3}));
    AISTUDIO_EXPECT(!(CapabilityVersion{1, 2, 3} == CapabilityVersion{1, 2, 4}));
}

AISTUDIO_TEST(CapabilityVersion_LessThan_ComparesMajorThenMinorThenPatch) {
    AISTUDIO_EXPECT((CapabilityVersion{1, 0, 0} < CapabilityVersion{2, 0, 0}));
    AISTUDIO_EXPECT((CapabilityVersion{1, 1, 0} < CapabilityVersion{1, 2, 0}));
    AISTUDIO_EXPECT((CapabilityVersion{1, 0, 1} < CapabilityVersion{1, 0, 2}));
    AISTUDIO_EXPECT(!(CapabilityVersion{2, 0, 0} < CapabilityVersion{1, 9, 9}));
}

AISTUDIO_TEST(Capability_Parse_VersionedString_SplitsNameAndVersion) {
    const auto capability = Capability::Parse("blueprint.read@2.1.0");
    AISTUDIO_EXPECT(capability.name == "blueprint.read");
    AISTUDIO_EXPECT(capability.version.has_value());
    AISTUDIO_EXPECT(capability.version->major == 2);
}

AISTUDIO_TEST(Capability_Parse_UnversionedString_KeepsWholeNameNoVersion) {
    const auto capability = Capability::Parse("diagnostics.ping");
    AISTUDIO_EXPECT(capability.name == "diagnostics.ping");
    AISTUDIO_EXPECT(!capability.version.has_value());
}

AISTUDIO_TEST(Capability_Parse_InvalidVersionSuffix_TreatsWholeStringAsName) {
    const auto capability = Capability::Parse("weird@notaversion");
    AISTUDIO_EXPECT(capability.name == "weird@notaversion");
    AISTUDIO_EXPECT(!capability.version.has_value());
}

AISTUDIO_TEST(ParseCapabilities_ParsesEachEntry) {
    const auto capabilities = ParseCapabilities({"a@1.0.0", "b"});
    AISTUDIO_EXPECT(capabilities.size() == 2);
    AISTUDIO_EXPECT(capabilities[0].name == "a");
    AISTUDIO_EXPECT(capabilities[1].name == "b");
}

AISTUDIO_TEST(IsCompatible_NulloptRequired_AlwaysSatisfied) {
    AISTUDIO_EXPECT(IsCompatible(std::nullopt, std::nullopt));
    AISTUDIO_EXPECT(IsCompatible(std::nullopt, CapabilityVersion{9, 9, 9}));
}

AISTUDIO_TEST(IsCompatible_RequiredButUnversionedProvided_Fails) {
    AISTUDIO_EXPECT(!IsCompatible(CapabilityVersion{1, 0, 0}, std::nullopt));
}

AISTUDIO_TEST(IsCompatible_SameMajor_ProvidedNewerOrEqual_Satisfied) {
    AISTUDIO_EXPECT(IsCompatible(CapabilityVersion{1, 0, 0}, CapabilityVersion{1, 0, 0}));
    AISTUDIO_EXPECT(IsCompatible(CapabilityVersion{1, 0, 0}, CapabilityVersion{1, 5, 0}));
    AISTUDIO_EXPECT(IsCompatible(CapabilityVersion{1, 2, 0}, CapabilityVersion{1, 2, 9}));
}

AISTUDIO_TEST(IsCompatible_SameMajor_ProvidedOlder_Fails) {
    AISTUDIO_EXPECT(!IsCompatible(CapabilityVersion{1, 5, 0}, CapabilityVersion{1, 0, 0}));
}

AISTUDIO_TEST(IsCompatible_DifferentMajor_Fails) {
    AISTUDIO_EXPECT(!IsCompatible(CapabilityVersion{1, 0, 0}, CapabilityVersion{2, 0, 0}));
    AISTUDIO_EXPECT(!IsCompatible(CapabilityVersion{2, 0, 0}, CapabilityVersion{1, 9, 9}));
}
