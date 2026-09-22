# コンテキスト削減ベースライン測定ハーネス

「Core（MCPサーバー）に繋いだ場合、繋がない場合と比べてどれだけコンテキスト消費が減るか」を、
**実際にClaude Codeを2通りに動かして、CLIが報告する実トークン数**で測る。

## 何を測っているか

タスクごとに、同じ自然文プロンプトを2通りの構成で実際にヘッドレス実行する（`claude -p ... --output-format json`）。

- **naive**: `--strict-mcp-config` でMCPサーバーを一切繋がず、`--tools Read,Grep,Glob` で自力調査させる。
- **core**: `context-reduction-core`（Core CLIをMCPサーバーとして起動）を繋ぎ、`--tools ""` で
  組み込みツールを全て外し、MCPのツールだけで同じ調査をさせる。Core側は `aistudio.config` の
  `mcp.tools` で `context_retrieve` / `context_fetch` / `symbol_search` に絞っている。

どちらも**実際にAPIを叩いた本物のセッション**で、3つの数字を出す。

| 指標 | 何か | 出どころ |
|---|---|---|
| cumulative | 全ターンのコンテキストの合計（APIが課金した量） | `--output-format json` の `modelUsage` を全モデル分合計 |
| final | 最終ターンのコンテキスト（statuslineが表示する値そのもの） | セッションのtranscript（`~/.claude/projects/<cwd>/<session>.jsonl`）の最後のassistantメッセージの `usage` |
| exploration | final − 初回ターンのベース（システムプロンプト＋ツール定義） | 同上 |

```
contextTokens = input_tokens + cache_creation_input_tokens + cache_read_input_tokens
```

この式はstatusline（`~/.claude/statusline.js`）と揃えてある。文字数からの推定（旧: chars/4）は使っていない。

**80%目標は exploration で判定する。** ベース（6〜8kトークン）は構成ごとに固定で、検索でどうにか
できる部分ではない。naiveの最終コンテキストが16kなら「80%削減=3.2k」はベースより小さく原理的に
不可能で、削減できるのは探索で増えた分だけ。

### MCPの固定オーバーヘッドが含まれる理由

`core`側の初回ターンのコンテキスト（= base）にCore MCPサーバーのツール定義がそのまま乗る。
`mcp.tools` で3つに絞った状態で、naive（Read/Grep/Glob）より小さい（約6.4k vs 7.8k）。

### サブエージェント消費が含まれる理由

`--output-format json`の`usage`はメインスレッド分のみだが、`modelUsage`は**実際に課金されたモデル
ごとに1エントリ**でき、サブエージェント（Task toolで起動される別モデルのセッション）の分もここに
独立して現れる。cumulativeは`modelUsage`を全モデル分合計しているので、サブエージェント経由の
消費も含まれる（`subagentsSpawned`も結果JSONに残る）。

### なぜAnthropic Console APIの`count_tokens`や組織Usage APIを使っていないか

どちらもAnthropic Console（従量課金のAPIプラットフォーム）のAPIキーが要り、Claude/Claude Codeの
Pro・Maxプラン（サブスクリプション）には含まれない。`claude -p`は既存の認証（Pro/MaxのOAuth）で
そのまま動くので、追加のAPIキーなしに実測値が取れる。結果JSONの`costUsd`は**従量課金だった場合の
参考換算額**で、Pro/Maxでは請求されない（プランの利用枠を消費するだけ）。

## 使い方

```
node bench/run.mjs
```

`build/Core/Debug/aistudio_core_cli.exe` が無ければ先にビルドする（README.md「Core のビルド」参照）。
別の場所にビルド済みなら `CORE_CLI_PATH` で指定できる。

**注意: 本物のClaude実行が `タスク数 x 2` 回走り、プランの利用枠を消費する。** 2タスクで約25万
トークン（cumulative合計）が目安。反復中は `BENCH_SAMPLE` で件数を絞ること:

```
BENCH_SAMPLE=2 node bench/run.mjs                              # 先頭2タスクだけ
BENCH_TASKS=security-sandbox,database-migrations node bench/run.mjs  # idで指定
BENCH_MODEL=sonnet node bench/run.mjs                          # モデルを固定して経時比較しやすくする
```

結果JSONはタスクが1つ終わるごとに上書き保存される（`complete: false`）。利用枠の上限で
`claude`がexit 1して途中で落ちても、そこまでの結果は残る。

結果は標準出力の表と、`bench/results/<timestamp>.json` に両方残る（naive/coreそれぞれの
`byModel`内訳・`toolCalls`・`sessionId`（transcriptを後から追える）も含む）。

## ハーネスの落とし穴（2026-09-18に実際に踏んだもの）

- `--allowedTools` は**許可リストであって除外ではない**。`bypassPermissions`下では組み込みツールが
  全部使えたままで、core側がRead/Grepで調べて`context_retrieve`を一度も呼ばなかった。
  組み込みツールを本当に外すのは `--tools`。
- Claude CodeのMCP接続タイムアウトは30秒、Coreの起動（インデックス構築）は約25秒。
  `MCP_TIMEOUT=120000` を環境変数で渡さないと、負荷次第で接続に失敗し、core側が
  「MCPツール無しのnaive」として静かに走る。Coreのログ（`%TEMP%/aistudio_mcp_<pid>.log`）に
  `context_retrieve: intent=` が出ているかで確認できる。

## タスクを増やす/変える

`bench/tasks.json` に `{id, intent}` を追加するだけ。`intent`はnaive/core両方の
プロンプトにそのまま埋め込まれる自然文。

## 注意

- LLMの調査経路は非決定的で、cumulativeは特にターン数に引きずられる。1回の実行の差はノイズを
  含むので `avg`/`median` だけでなく `min`/`max` も見る。final/explorationの方が安定している。
- `bench/results/` にJSONが溜まっていくので、80%目標に対する現在値はそこを時系列で見ればよい。
