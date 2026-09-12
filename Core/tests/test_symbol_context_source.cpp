#include "test_framework.hpp"
#include "Core/Context/SymbolContextSource.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(MakeSymbolContextItem_SetsSourceAndId) {
    Symbol symbol;
    symbol.name = "Player::Attack";
    symbol.kind = SymbolKind::Function;
    symbol.file_path = "src/Player.cpp";
    symbol.line = 42;

    const auto item = MakeSymbolContextItem(symbol);

    AISTUDIO_EXPECT(item.source == ContextSourceKind::Symbol);
    AISTUDIO_EXPECT(item.id == "src/Player.cpp:Player::Attack");
    AISTUDIO_EXPECT(item.content.find("Player::Attack") != std::string::npos);
    AISTUDIO_EXPECT(item.content.find("42") != std::string::npos);
    AISTUDIO_EXPECT(item.estimated_tokens > 0);
}

AISTUDIO_TEST(MakeSymbolContextItem_SetsCompressionToSymbol) {
    Symbol symbol;
    symbol.name = "Foo";
    symbol.file_path = "a.hpp";
    const auto item = MakeSymbolContextItem(symbol);
    AISTUDIO_EXPECT(item.compression == CompressionLevel::Symbol);
}

AISTUDIO_TEST(MakeSymbolContextItem_DefaultPriority_IsSixty) {
    Symbol symbol;
    symbol.name = "Foo";
    symbol.file_path = "a.hpp";
    const auto item = MakeSymbolContextItem(symbol);
    AISTUDIO_EXPECT(item.priority == 60);
}

AISTUDIO_TEST(MakeSymbolContextItem_CustomPriority_IsRespected) {
    Symbol symbol;
    symbol.name = "Foo";
    symbol.file_path = "a.hpp";
    const auto item = MakeSymbolContextItem(symbol, 90);
    AISTUDIO_EXPECT(item.priority == 90);
}

AISTUDIO_TEST(MakeSymbolContextItem_WithSignature_AppendsIt) {
    Symbol symbol;
    symbol.name = "Foo::Bar";
    symbol.file_path = "a.cpp";
    symbol.signature = "int Foo::Bar(int x) const";
    const auto item = MakeSymbolContextItem(symbol);
    AISTUDIO_EXPECT(item.content.find("int Foo::Bar(int x) const") != std::string::npos);
}

AISTUDIO_TEST(MakeSymbolContextItem_WithoutSignature_OmitsIt) {
    Symbol symbol;
    symbol.name = "Foo";
    symbol.file_path = "a.hpp";
    const auto item = MakeSymbolContextItem(symbol);
    AISTUDIO_EXPECT(item.content.find('\n') == std::string::npos);
}
