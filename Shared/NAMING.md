# 命名法則

## 1. 目的
このドキュメントは、プロジェクト内で使用する命名規則を統一するためのガイドラインを定義する。

---

## 2. 基本ルール
- 名前は意味が分かるものにする
- 略語の多用は避ける
- 単数形・複数形を統一する
- 命名から責務が推測できるようにする
- 英語で統一する（コメントやドキュメントは日本語可）

---

## 3. 命名スタイル
# 基本的にパスカルケースを使用

| 対象 | スタイル | 例 |
|------|---------|----|
| 変数 | PascalCase | `UserName` |
| 関数 | PascalCase | `FetchUserData()` |
| クラス | PascalCase | `UserRepository` |
| 定数 | UPPER_SNAKE_CASE | `MAX_RETRY_COUNT` |
| ファイル | PascalCase | `UserService.ts` |
| ディレクトリ | PascalCase | `UserManagement/` |

---

## 4. 用途別命名ルール

### 4.1 ブール値
- 接頭辞に `is` / `has` / `can` / `should` を使用する  
**例:** `isActive`, `hasPermission`, `canEdit`

### 4.2 関数
- 動詞から始める
- 処理内容が分かるようにする  

**推奨プレフィックス:** `Get`, `Fetch`, `Create`, `Update`, `Delete`, `Validate`, `Convert`  

**例:** `FetchUserProfile()`, `ValidateToken()`, `ConvertDateFormat()`

### 4.3 コレクション
- 配列やリストは複数形を使用する  
**例:** `Users`, `Products`, `OrderItems`

---

## 5. ドメイン別命名ルール

### 5.1 データベース
| 対象 | ルール | 例 |
|------|-------|----|
| テーブル | PascalCase 複数形 | `UserProfiles` |
| カラム | PascalCase | `CreatedAt` |
| 主キー | `Id` | `Id` |
| 外部キー | `{Table}Id` | `UserId` |

### 5.2 API
- エンドポイントは kebab-case
- リソース指向で設計する  

### 5.3 ブランチ

#### 基本ルール

* ブランチ名は `{type}/{Name}` 形式で作成する
* 種別ごとにプレフィックスを付与する
* 機能名・修正内容は PascalCase を使用する
* 内容が分かる命名にする

#### ブランチ種別

| 種別       | 用途       | 例                         |
| ---------- | -------- | ------------------------- |
| `feature`  | 新機能追加    | `feature/AddLoginSystem`  |
| `fix`      | バグ修正     | `fix/LoginValidation`     |
| `refactor` | リファクタリング | `refactor/UserService`    |
| `hotfix`   | 緊急修正     | `hotfix/FixCrashOnStart`  |
| `docs`     | ドキュメント修正 | `docs/UpdateReadme`       |
| `test`     | テスト追加・修正 | `test/AddUserServiceTest` |

---

## 6. 禁止パターン
- 意味のない省略
- 1文字変数（ループ変数は除く）
- 文脈依存の名前
- 長すぎる名前

**例（悪い例）:** `tmp`, `data2`, `func`, `aaa`

---

## 7. その他ルール

### DTO
- `UserDto`
- `CreateUserRequest`

### Service
- `UserService`

### Repository
- `UserRepository`

### Controller
- `UserController`
