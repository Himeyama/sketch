# www

BLE 経由で Wi-Fi を設定し、接続後は HTTP サーバーとして `index.html` を配信する ESP32 スケッチ。

```
www.ino          スケッチ本体
setup.html       PC / スマホでローカルに開く設定ページ (Web Bluetooth)
data/index.html  LittleFS に書き込み、ESP32 が配信するページ
compile.ps1      コンパイル + LittleFS イメージ作成
upload.ps1       ビルドして ESP32 に書き込む
tools.ps1        上記 2 つが使うツール探索ヘルパー
```

## 動作の流れ

1. ESP32 が `ESP32-WWW` として BLE アドバタイズする。
2. `setup.html` をブラウザで開き「ESP32 に接続する」→ デバイスを選ぶ。
   - BLE 接続が確立した時点で **GPIO2 が HIGH** になる（切断で LOW に戻る）。
3. 管理パスワードでログインする。認証するまで Wi-Fi 系の書き込みは無視される。
4. SSID とパスワードを入力（「スキャン」で周辺の AP を一覧できる）して「保存して接続」。
   - 設定は NVS に保存され、次回以降は起動時に自動で接続する。
5. Wi-Fi 接続が完了すると ESP32 が HTTP サーバーを起動し、設定ページに `http://<IP>/` へのリンクが出る。
   - `/` → `data/index.html`、`/api/status` → 稼働状況の JSON。

BLE は Wi-Fi 接続後も動き続けるので、いつでも設定をやり直せる。

## 書き込み手順

```powershell
cd ESP32\www
.\upload.ps1       # ビルドして書き込む (ポートは自動検出)
```

`upload.ps1` は書き込みの前に `compile.ps1` を呼び、**書き込むものだけをビルドする**。
ビルドが失敗した場合はデバイスに触れずに終わるので、古いファームウェアがそのまま残る。
`compile.ps1` を単体で実行すれば書き込まずにビルドだけできる。

スクリプトは Arduino IDE 2.x が同梱する `arduino-cli` と、ESP32 コアに付属する
`mklittlefs` / `esptool` を自動で探す。個別にインストールしなくてよい。

`data/index.html` はスケッチとは別のパーティションに置かれるので、転送も別扱いになる。
`upload.ps1` は既定で両方書き込むが、HTML だけ直したときは `-FilesystemOnly` が速い
(スケッチのコンパイルを飛ばし、`littlefs.bin` だけ作り直す)。
転送し忘れると、シリアルに `LittleFS mount failed` が出るか `/` が 404 になる。

| 用途 | コマンド |
| --- | --- |
| ポートを指定 | `.\upload.ps1 -Port COM5` |
| HTML だけ更新 | `.\upload.ps1 -FilesystemOnly` |
| スケッチだけ更新 | `.\upload.ps1 -SketchOnly` |
| Wi-Fi 設定と管理パスワードを初期化 | `.\upload.ps1 -Erase` |
| ビルドをやり直す | `.\upload.ps1 -Clean` |
| ビルドせず build\ の成果物を書き込む | `.\upload.ps1 -SkipCompile` |
| 書き込まずにビルドだけ | `.\compile.ps1` |

**Partition Scheme は `huge_app` を使う。** BLE と Wi-Fi を同居させるとスケッチが
約 1.73MB になり、既定の 1.3MB には収まらない。両スクリプトの `-Fqbn` の既定値
`esp32:esp32:esp32:PartitionScheme=huge_app` がこれにあたる。別のボードや
スキームを使うなら compile と upload に**同じ** `-Fqbn` を渡すこと
（LittleFS のオフセットとサイズはパーティション表から読むので、食い違うと壊れる）。

Arduino IDE から手で行う場合は、Partition Scheme を
`Huge APP (3MB No OTA/1MB SPIFFS)` にしたうえで、`data/` の転送に
[arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload)
プラグインが要る。

### setup.html を開く

Web Bluetooth が必要なので **Chrome / Edge** を使う（Safari・Firefox は非対応）。
`file://` でそのまま開ける。Android の Chrome では位置情報の許可が要ることがある。

## 管理パスワード

初期値は `www.ino` の `DEFAULT_ADMIN_PASSWORD`（`esp32admin`）。
初回起動時に NVS へ保存され、以降は設定ページの「管理パスワードを変更」から変えられる。
NVS の値が優先されるので、スケッチの定数を書き換えても既に保存済みなら反映されない。
初期化するには `.\upload.ps1 -Erase`（保存済みの Wi-Fi 設定も一緒に消える）。

## 設計上の注意

- **BLE のコールバックは BLE タスクから呼ばれる。** フラッシュ書き込みや `WiFi.scanNetworks()`
  をそこで実行すると BLE スタックが止まるため、コールバックはフラグを立てるだけにして
  実処理は `loop()` に寄せてある。この非対称性のせいで、ブラウザは書き込んだ直後に読み返しても
  まだ古い値を得る。`setup.html` が結果をステータス通知で待ち合わせているのはこのため。
- ステータスは READ + NOTIFY だが、通知は MTU で切り詰められうる。通知は「変化した」合図として
  だけ使い、値は毎回 read で取り直している（long read なので全長が読める）。
- 認証状態はグローバルに 1 つで、BLE 切断時にリセットされる。複数クライアントが同時に
  接続した場合、片方の認証が他方にも効いてしまう。
- Wi-Fi パスワードは BLE の暗号化なしで平文送信され、NVS にも平文で保存される。
  信頼できない環境で使うなら BLE のペアリング/暗号化と NVS 暗号化を有効にすること。
- HTTP サーバー側に認証はない。同じ LAN の誰でも `/` と `/api/status` を見られる。

## BLE サービス

Service `7d2ec300-4a9b-4d6e-9f31-08c7a5be1d40`

| 特性 | UUID 末尾 | 権限 | 内容 |
| --- | --- | --- | --- |
| Auth | `...301` | Write | 管理パスワード |
| SSID | `...302` | Write | 接続先 SSID |
| Password | `...303` | Write | Wi-Fi パスワード |
| Control | `...304` | Write | `scan` / `connect` / `disconnect` / `forget` / `setpw:<新パスワード>` |
| Status | `...305` | Read, Notify | `{"auth","wifi","savedSsid","ip","rssi"}` |
| Scan | `...306` | Read | `[{"ssid","rssi","open"}, ...]` |

`wifi` は `idle` / `scanning` / `connecting` / `connected` / `failed`。
Auth 以外の Write は未認証だと黙って捨てられる。

`connect` は直前に SSID を書いていればそれを保存して使い、書いていなければ保存済みの
設定で繋ぐ。`disconnect` は認証情報を残したまま切り、自動再接続も止める。`forget` は
認証情報ごと消す。
