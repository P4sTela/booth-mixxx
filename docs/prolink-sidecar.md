# PRO DJ LINK sidecar プロトコル（v0）

Mixxx フォークと、booth の Rust sidecar の間の契約。sidecar は MIT OR Apache-2.0 の別プロセスで、
PRO DJ LINK と暗号化 `master.db` を扱う。このアプリ（GPL）はクライアントだけを持つ。

- 実装（予定）: booth の `pdl sidecar`（`crates/pdl`）
- 状態: **v0 の草案**。最初の実装（`pdl sidecar` と、このアプリの `library.open` / `library.tracks`）を
  作りながら確定させる。変えたらこの文書と `protocol` の値を上げる。

## プロセスと transport

- 別プロセス。このアプリが起動し、終了時に閉じる。
- 実行ファイル: 設定 `sidecar.binary`（既定は `PATH` の `pdl`）。引数 `sidecar --socket <path>`。
- transport: Unix domain socket（macOS / Linux）。パスは設定 `sidecar.socket`、既定は
  `$TMPDIR/pdl-sidecar.sock`。Windows は TCP `127.0.0.1` の動的ポート + ポート番号を書いたファイル（v1）。
- 1 接続だけを想定する。クライアントが切れたら sidecar は次の接続を待つ（終了しない）。
- ソケットは `0600`、所有者は実行ユーザー。ネットワークには出さない。

## framing

- UTF-8 の JSON を 1 行 1 メッセージ（NDJSON）。改行までが 1 メッセージ。
- 1 メッセージの上限は 16 MiB。超えたら `error` を返して接続を切る。
- バイナリ（ANLZ など）は JSON に埋めない。sidecar が一時ファイルへ書き、パスを返す。

## メッセージ

### 要求

```json
{"id": 1, "method": "library.open", "params": {"db": null}}
```

`id` はクライアントが決める整数（接続内で一意）。`params` は省略可。

### 応答

```json
{"id": 1, "result": {"tracks": 409, "playlists": 12}}
```

```json
{"id": 1, "error": {"code": "library.not_found", "message": "master.db が見つからない"}}
```

### イベント（要求への応答ではない）

```json
{"event": "link.deck", "data": {"number": 1, "playing": true, "bpm": 128.0}}
```

`id` を持たない。v0 ではイベントを送らない（`hello` の `capabilities` に載ったものだけ）。

## ハンドシェイク

最初の要求は必ず `hello`。

```json
{"id": 1, "method": "hello", "params": {"protocol": "pdl-sidecar/0"}}
```

```json
{"id": 1, "result": {
  "protocol": "pdl-sidecar/0",
  "sidecar": "0.1.0",
  "capabilities": ["library", "library.analysis"]
}}
```

`protocol` が一致しなければクライアントは接続を閉じる。`capabilities` に無い method は呼ばない。

## v0 の method

| method | params | result |
|---|---|---|
| `hello` | `protocol` | `protocol`, `sidecar`, `capabilities` |
| `library.open` | `db`（パス。`null` なら既定の場所を探す） | `tracks`, `playlists` |
| `library.tracks` | `offset`（既定 0）, `limit`（既定 500、最大 5000） | `total`, `tracks[]` |
| `library.track` | `id` | 下記の 1 件（フィールドは `tracks[]` と同じ） |
| `library.analysis` | `id` | `anlz: {dat, ext}`（書き出したファイルの絶対パス） |

### track

```json
{
  "id": 1234,
  "title": "…",
  "artist": "…",
  "album": "…",
  "genre": "…",
  "bpm": 12800,
  "duration_ms": 321000,
  "key": "8A",
  "path": "/Users/…/track.aiff",
  "bitrate": 1411,
  "sample_rate": 44100,
  "date_added": "2026-01-02T03:04:05Z"
}
```

- `bpm` は rekordbox の保存値（BPM×100）をそのまま返す。クライアントが 100 で割る。
- `path` は元の音声ファイル。存在しない場合も返す（クライアントが「見つからない」と表示できるように）。
- `library.analysis` の `dat` / `ext` は、sidecar のキャッシュ（`$TMPDIR/pdl-sidecar-cache/<id>/`）に
  書き出した ANLZ。無い場合は `null`。

## エラー

| code | 意味 |
|---|---|
| `bad_request` | JSON でない、`id` が無い、引数の型が違う |
| `unknown_method` | 未知の method |
| `library.not_found` | `master.db`（と `-wal`）が見つからない |
| `library.locked` | コピーに失敗した（読み取り中のロックなど） |
| `library.bad_db` | 復号・スキーマの読み取りに失敗した |
| `track.not_found` | `id` が無い |
| `internal` | 上記以外。`message` に詳細 |

## ライフサイクルと失敗時

1. アプリはソケットに接続。失敗したら sidecar を起動して 2 秒間隔で再試行（最大 10 秒）。
2. `hello` が通らなければ sidecar を終了させ、機能を無効にする（アプリは動き続ける）。
3. 接続が切れたら 1 回だけ再接続する。`library.open` はクライアントが再度呼ぶ。
4. sidecar が落ちてもアプリは落とさない。UI に「PRO DJ LINK: 未接続」を出す。

## 禁止・非目標

- リクエストから任意のファイルを読む method を作らない（パスを返すだけ）。
- `master.db` はコピーだけを読む。原本・`-wal` を直接開かない（booth の既存方針）。
- GPL のコードを sidecar に持ち込まない。逆も同じ。
- v0 では PRO DJ LINK の受信イベントを扱わない（`library` だけ）。デッキ状態は v1。
