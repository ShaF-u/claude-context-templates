# コンテキスト削減ベースライン測定ハーネス

「Core（MCPサーバー）に繋いだ場合、繋がない場合と比べてどれだけコンテキスト消費が減るか」を測る。

## 何を測っているか

タスクごとに2通りの方法で同じ情報を得た場合のトークン数を比較する。

- **naive**: 素のエージェントがCoreなしでやること — `task.keywords` で `git grep` して、ヒットしたファイルを丸ごと読む想定でトークン数を積算する。
- **core**: `context_retrieve` ツールに `task.intent` を渡して返ってきたJSON応答のトークン数。

トークン数は簡易推定（文字数/4）。両辺に同じ推定を使うので、比率としての削減率は意味を持つが、絶対値は目安。

## 使い方

```
node bench/run.mjs
```

`build/Core/Debug/aistudio_core_cli.exe` が無ければ先にビルドする（README.md「Core のビルド」参照）。別の場所にビルド済みならは `CORE_CLI_PATH` で指定できる。

結果は標準出力の表と、`bench/results/<timestamp>.json` に両方残る。

## タスクを増やす/変える

`bench/tasks.json` に `{id, intent, keywords}` を追加するだけ。`keywords` はnaive側のgit grep対象、`intent` はcontext_retrieve側に渡す自由文。

## 注意

- これは「選別(Selection)層」の効果測定。圧縮層（ContextCompressor）は現状head/tail切り出しのみで、SemanticSummary(LLM要約)は未実装 — その分の削減効果はここには出ない。
- naive側の `keywords` は手で選んでいるので恣意性が残る。タスクを増やすほど平均値は信頼できるようになる。
- 80%目標に対する現在値の追跡は、`bench/results/` にJSONが溜まっていくので、そこを時系列で見ればよい（git管理下に置くかは運用次第 — 差分が追いやすいので置く方を推奨）。
