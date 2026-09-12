#include "test_framework.hpp"
#include "Core/Error/Result.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(Result_Ok_HoldsValue) {
    const auto r = Result<int>::Ok(42);
    AISTUDIO_EXPECT(r.IsOk());
    AISTUDIO_EXPECT(r.Value() == 42);
}

AISTUDIO_TEST(Result_Fail_HoldsError) {
    const auto r = Result<int>::Fail(Error{.code = ErrorCode::NotFound, .message = "missing"});
    AISTUDIO_EXPECT(r.IsError());
    AISTUDIO_EXPECT(r.Err().code == ErrorCode::NotFound);
}

AISTUDIO_TEST(Result_Void_Ok) {
    const auto r = Result<void>::Ok();
    AISTUDIO_EXPECT(r.IsOk());
}

AISTUDIO_TEST(Result_Void_Fail) {
    const auto r = Result<void>::Fail(Error{.code = ErrorCode::Internal, .message = "boom"});
    AISTUDIO_EXPECT(r.IsError());
    AISTUDIO_EXPECT(r.Err().message == "boom");
}

AISTUDIO_TEST(ToString_ErrorCode_CoversAllValues) {
    AISTUDIO_EXPECT(ToString(ErrorCode::Unknown) == "Unknown");
    AISTUDIO_EXPECT(ToString(ErrorCode::NotFound) == "NotFound");
    AISTUDIO_EXPECT(ToString(ErrorCode::InvalidArgument) == "InvalidArgument");
    AISTUDIO_EXPECT(ToString(ErrorCode::IOError) == "IOError");
    AISTUDIO_EXPECT(ToString(ErrorCode::ParseError) == "ParseError");
    AISTUDIO_EXPECT(ToString(ErrorCode::Timeout) == "Timeout");
    AISTUDIO_EXPECT(ToString(ErrorCode::PermissionDenied) == "PermissionDenied");
    AISTUDIO_EXPECT(ToString(ErrorCode::Cancelled) == "Cancelled");
    AISTUDIO_EXPECT(ToString(ErrorCode::Internal) == "Internal");
}
