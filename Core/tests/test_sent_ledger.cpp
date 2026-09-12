#include "test_framework.hpp"
#include "Core/Context/SentLedger.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(SentLedger_Check_UnknownId_ReportsNotUnchanged) {
    SentLedger ledger;
    ledger.BeginResponse();
    const auto result = ledger.Check("src/main.cpp", "content");
    AISTUDIO_EXPECT(!result.unchanged);
}

AISTUDIO_TEST(SentLedger_Check_SameContentAfterRecordSent_ReportsUnchanged) {
    SentLedger ledger;
    const auto first_ordinal = ledger.BeginResponse();
    ledger.RecordSent("src/main.cpp", "content v1");

    ledger.BeginResponse();
    const auto result = ledger.Check("src/main.cpp", "content v1");

    AISTUDIO_EXPECT(result.unchanged);
    AISTUDIO_EXPECT(result.sent_at_ordinal == first_ordinal);
}

AISTUDIO_TEST(SentLedger_Check_ChangedContent_ReportsNotUnchanged) {
    SentLedger ledger;
    ledger.BeginResponse();
    ledger.RecordSent("src/main.cpp", "content v1");

    ledger.BeginResponse();
    const auto result = ledger.Check("src/main.cpp", "content v2");

    AISTUDIO_EXPECT(!result.unchanged);
}

AISTUDIO_TEST(SentLedger_RecordSent_AfterContentChange_UpdatesOrdinalToWhenActuallySent) {
    SentLedger ledger;
    ledger.BeginResponse();
    ledger.RecordSent("src/main.cpp", "content v1");

    ledger.BeginResponse();
    // Simulates a resend of unchanged content: Check() alone must not
    // move the recorded ordinal forward (only RecordSent() does).
    const auto unchanged_check = ledger.Check("src/main.cpp", "content v1");
    AISTUDIO_EXPECT(unchanged_check.unchanged);

    const auto third_ordinal = ledger.BeginResponse();
    ledger.RecordSent("src/main.cpp", "content v2");

    ledger.BeginResponse();
    const auto result = ledger.Check("src/main.cpp", "content v2");
    AISTUDIO_EXPECT(result.unchanged);
    AISTUDIO_EXPECT(result.sent_at_ordinal == third_ordinal);
}

AISTUDIO_TEST(SentLedger_Check_DoesNotMutateState) {
    SentLedger ledger;
    ledger.BeginResponse();
    ledger.RecordSent("src/main.cpp", "content v1");

    ledger.BeginResponse();
    const auto first_check = ledger.Check("src/main.cpp", "content v1");
    const auto second_check = ledger.Check("src/main.cpp", "content v1");
    const auto result = ledger.Check("src/main.cpp", "content v1");
    AISTUDIO_EXPECT(first_check.unchanged);
    AISTUDIO_EXPECT(second_check.unchanged);

    AISTUDIO_EXPECT(result.unchanged);
    AISTUDIO_EXPECT(result.sent_at_ordinal == 1);
}

AISTUDIO_TEST(SentLedger_DifferentIds_AreIndependent) {
    SentLedger ledger;
    ledger.BeginResponse();
    ledger.RecordSent("a.cpp", "same");

    ledger.BeginResponse();
    const auto a_result = ledger.Check("a.cpp", "same");
    const auto b_result = ledger.Check("b.cpp", "same");

    AISTUDIO_EXPECT(a_result.unchanged);
    AISTUDIO_EXPECT(!b_result.unchanged);
}
