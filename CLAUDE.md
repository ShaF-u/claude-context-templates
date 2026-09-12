# claude-context-templates — Read This First

このリポジトリは2つの役割を持つ。

1. **テンプレート集**（`AI/` `Docs/` `Shared/`）— 他のプロジェクトへコピーして使う、
   コンテキスト削減用のMarkdownテンプレート。中身はプレースホルダのまま（このリポジトリ自身の
   構造を説明するものではない）。使い方は `README.md` 参照。
2. **Core**（`Core/`）— 大規模プロジェクト向けのMCPサーバー（C++）。symbol_search/
   context_retrieve等、索引ベースでコードの関連部分だけを取得するツールを提供する。
   ClaudeGuiというプロジェクトで開発していたContext EngineからGUI部分を除いて移植したもの。
   詳細は `README.md`「Core（MCPサーバー）」参照。

## このリポジトリ自身の構造

```
claude-context-templates/
├── AI/       ... AI向けテンプレート（STRUCTURE.md, DECISIONS.md）
├── Docs/     ... 人間向け（現状空）
├── Shared/   ... 人間・AI両方向け（NAMING.md, CONVENTIONS.md）
├── Core/     ... MCPサーバー本体（C++、CMakeプロジェクト）
├── README.md
└── CLAUDE.md ... このファイル
```

## Core のビルド

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target aistudio_core_cli aistudio_core_tests
```

ビルド後、`.mcp.json` により `context-reduction-core` としてMCP接続できる（初回承認が必要）。

## 作業時の注意

- `Core/` 内のコメントは元プロジェクト（ClaudeGui）由来で、`docs/ROADMAP.md` や
  `AGENT.md` など**このリポジトリには存在しないファイル**への参照が残っている箇所がある。
  機能には影響しないが、混乱しないよう注意（全面的に書き換える予定はない — 労力に見合わない）。
- `AI/` `Shared/` 配下のファイルはテンプレート（プレースホルダ入り）。このリポジトリ自身の
  設定として書き換えない——他プロジェクトへコピーする際の元の形を保つ。
