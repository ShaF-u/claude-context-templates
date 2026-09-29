# コンテキスト削減ベースライン測定ハーネス

「Core（MCPサーバー）に繋いだ場合、繋がない場合と比べてどれだけコンテキスト消費が減るか」を、
**実際にClaude Codeを2通りに動かして、CLIが報告する実トークン数**で測る。

## 何を測っているか

タスクごとに、同じ自然文プロンプトを2通りの構成で実際にヘッドレス実行する（`claude -p ... --output-format json`）。

- **naive**: `--strict-mcp-config` でMCPサーバーを一切繋がず、`--tools Read,Grep,Glob` で自力調査させる。
- **core**: `context-reduction-core`（Core CLIをMCPサーバーとして起動）を繋ぎ、`--tools ""` で
  組み込みツールを全て外し、MCPのツールだけで同じ調査をさせる。Core側は `aistudio.config` の
  `mcp.tools` で `context_retrieve` / `context_fetch` / `symbol_search` / `keyword_search` に絞っている。

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
`mcp.tools` で4つに絞った状態で、naive（Read/Grep/Glob）より小さい（約6.4k vs 7.8k）。

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
BENCH_REPEAT=3 node bench/run.mjs                              # 各サイド3回走らせて中央値を比較
BENCH_MODEL=sonnet node bench/run.mjs                          # モデルを固定して経時比較しやすくする
```

**`BENCH_REPEAT=1`(既定)の単発比較で変更の良し悪しを判断しないこと。** 同じタスクを
同じ設定で走らせても、naive側で最大3.9倍・core側で最大6.5倍ぶれる（モデルの探索経路が
非決定的なため。`bench/results/`の履歴で確認済み、2026-09-22）。10〜30%の差は
このノイズに完全に埋もれる。効果を見るなら `BENCH_REPEAT=3` 以上。

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

## このベンチが測っていないもの（重要）

**回答の正しさを一切測っていない。** naive/core のどちらも、最終回答が正しいか、
引用したfile:lineが妥当かは検証していない。したがって「応答を削ってトークンを
減らす」方向の変更は、答えが劣化しても数値上は改善に見える。

実例として、2026-09-24時点の最良タスク(context-selection-budget)の内訳:

```
naive中央値 72,208 / core 17,485 = 75.8%削減 (core側は1回の呼び出しで回答)
  coreの内訳: ベース(2ターン x 5,400) 10,800 + 応答等 6,685
  80%に必要なcore: 14,442 → 応答等を 3,642 まで圧縮する必要
  つまり応答を 17,226文字 → 約5,000文字(-71%)
```

一方で、その応答のうち答えが実際に引用しなかったのは約1,728文字しかない。
残りを削るには**答えが使っている内容を削る**ことになり、このベンチはその劣化を
検出できない。数値を追ってここへ踏み込まないこと。

80%を正当に狙うなら、ベンチを甘くするのではなく、**このツールが本来対象とする
規模のコードベース**(README.md「大規模プロジェクト向け」)で測るべき。naiveの
ターン数が増えるほど上限 1-(coreターン/naiveターン) は上がる。

## ペイロードの内訳（2026-09-29、次にやること）

GameEngineを対象にした実測で、core側は1呼び出しで答えても naive に負けることが確定した。

```
model-binary-load: core 1呼び出し(理論下限)
  core  base 7,814 / final 14,682 / exploration 6,868
  naive base 8,935 / final 14,534 / exploration 5,599  (3呼び出し)
```

ターン数の問題(1-(coreターン/naiveターン))は解けている。残るのは1回あたりの
ペイロードサイズだけ。CLIに直接JSON-RPCを流して内訳を測ると(Claude実行不要=無料)、
大半がノイズだった:

| タスク | ノイズ | 割合 |
|---|---|---|
| dx12-device-init | `.vcxproj` のkeywordヒット4件(7.9KB) + 無関係な Mesh.cpp/ImGuiManager.cpp(9.3KB) | 72% |
| dx12-descriptor-heap | `.vcxproj` / `.vcxproj.filters` 3件 | 23% |
| model-binary-load | CLAUDE.md のkeywordヒット2件(3.4KB) + そこ由来の見出し3つ | 28% |

`.vcxproj` はビルド定義XMLで、どの質問の答えにも使われていない。CLAUDE.md は
Claude Codeがセッション開始時に読むので純粋な重複 — `backend.core.project_rules.files=`
で止めたはずが keyword_search 経路から入り込んでいた。

### 実施結果（2026-09-29、`node bench/payload.mjs` でオフライン計測）

`bench/configs/gameengine.config` の `scan.extra_ignore_patterns` に
`*.vcxproj,*.vcxproj.filters,*.sln,CLAUDE.md` を追加した。**単独では総量 -2% しか
減らなかった**: 除外で空いた枠を、別の無関係なファイル（ImGuiManager.cpp など）が
埋めた。dx12-descriptor-heap はむしろ +49% になった。原因は、キーワードヒットの
全文昇格に件数上限もカットオフもなかったこと。カットオフの下で「呼び出し元」として
例外的に残るだけの弱いヒットまで、ファイル全体を連れてきていた。

`ContextRetriever` を修正し、キーワードヒットの全文昇格は、Relative cutoff を自力で
通過する強さのヒットに限った。弱いヒットは13行の窓に戻る。

| 対象 | ベースライン | 除外パターン追加 | ＋全文昇格の修正 |
|---|---|---|---|
| GameEngine 8タスク | 120,645 chars | 118,249 (-2.0%) | 111,969 (**-7.2%**) |
| Core 15タスク | 263,464 | — | 241,525 (**-8.3%**) |

残っている最大のノイズは dx12-device-init の ImGuiManager.cpp / Mesh.cpp（計9.4KB）。
どちらも "device" の実使用箇所で、スコア上はカットオフを通過する強いヒットになる。
削るには順位付けそのものに手を入れる必要がある。**ここまでは実行コストゼロの
オフライン計測で、Claudeを使った本ベンチ（`BENCH_REPEAT=3`）はまだ回していない。**

## 計測対象が動く問題（重要）

このベンチのタスクは `Core/` 自身について問うので、**Coreを変更すると計測対象も変わる**。
実例（2026-09-23）: 一連の改善作業で `Core/src/Context/ContextRetriever.cpp` が
897行 → 1,057行(+18%)に増え、「context items のランク付けと予算」を問うタスクは
その分だけ重くなった。core側の応答にはその増えたファイルの一部が入るので、影響は
naive側より大きく出る。

したがって:

- **同一実行内の naive vs core の比較は有効**（同じリポジトリ状態を見ている）。
- **同じタスクの実行間の比較は交絡している**。「改善した/悪化した」を判断するには、
  リポジトリを固定するか、`git stash` で変更を退避した状態と当てた状態を同じ時点で
  測るしかない。
- 本気で経時比較するなら、対象を別リポジトリ（変更しないもの）にするべき。

## 注意

- LLMの調査経路は非決定的で、cumulativeは特にターン数に引きずられる。1回の実行の差はノイズを
  含むので `avg`/`median` だけでなく `min`/`max` も見る。final/explorationの方が安定している。
- `bench/results/` にJSONが溜まっていくので、80%目標に対する現在値はそこを時系列で見ればよい。
