# STCC 勝手移植プロジェクト — 引継ぎドキュメント

最終更新: 2026-09-13（Phase 0 完了 / Phase 1 ほぼ完了）
このドキュメントは Claude Code / Cowork で作業を再開する際の起点です。
新しいセッションを始めたら、まずこれを読ませてください。

---

## 1. プロジェクト概要

**Sega Touring Car Championship (STCC) の Windows版を土台に、Windows 11 で動かし、機能拡張MODを作る。**

個人の趣味プロジェクト。商用利用なし。
アセット・ディスクイメージ・改変バイナリの再配布は行わない（詳細は §6）。

### 対象作品の基礎情報

| 項目 | 内容 |
|---|---|
| 原典 | アーケード版 1996年 / Sega Model 2C CRX基板 |
| 開発 | Sega AM Annex（AM3系）、プロデューサー 水口哲也、ディレクター 佐々木健二 |
| 移植 | Sega Saturn版 1997年11月 / Windows版 1998年1〜2月 |
| 本プロジェクトの対象 | **Windows版 日本版 v1.02**（ディスク同梱パッチ適用後） |
| 収録車両 | Alfa Romeo 155 V6 TI / Toyota Supra GT / AMG Mercedes C-Class / Opel Calibra V6 ＋隠し車両 |
| コース | 3コース＋隠しコース |

**重要な前提: このゲームには既にリプレイ機能が存在し、`REPLAY/REPLAY%02d.DAT` としてファイル保存される。** リプレイのデータ記録・再生機構はゲーム側に既にあり、ゼロから状態記録を実装する必要がない。

### なぜアーケード版ではなくWindows版か

アーケード版はソースコード非公開、i960系CPU＋専用ジオメトリプロセッサという構成で、Ghidraのプロセッサ対応すら心もとない。対してWindows版はx86バイナリなので解析ツールが全部揃う。

---

## 2. 達成したいこと（優先度順）

0. **Windows 11 で安定して起動・プレイできる**
1. **ホイール対応**
2. **ウルトラワイドスクリーン化**（21:9 / 32:9）
3. **リプレイの自由カメラ**（複数視点・シネマティック）

この順番は難易度順でもあり、かつ後段が前段の成果を再利用する依存関係になっている。特に 2 で特定する投影まわりの知識が、3 のカメラ制御にそのまま効く。

---

## 3. アーキテクチャ決定

### 決定1: 基準版は v1.02
- v1.00 は DirectDraw のみ（ソフトウェア描画）。v1.02 で Direct3D（DX5, `IDirect3D2`）が追加され、アナログ入力のゴースト不具合も修正されている
- DLL は exe の SHA-256 で版を判定し、`data/addresses/versions.toml` に `supported = true` の版にだけパッチを当てる

### 決定2: 成果物は exe への hex パッチではなく、ラッパーDLL
理由:
- 差分管理ができる（hex編集は履歴が残らない）
- 原盤のバージョン違いに対応しやすい
- **改変exeは二次的著作物だが、DLLは自分のオリジナルコード。** 将来公開する気になった場合、DLLだけは配布できる

hex編集は調査段階の使い捨てに限定し、確定した変更はDLL側へ移すこと。

### 決定3: DLL の注入口は `dinput.dll` プロキシ（ddraw.dll ではない）
- `ddraw.dll` は dgVoodoo2 が使うので衝突させない
- `dinput.dll` プロキシでプロセスに入り、DirectDraw / Direct3D は MinHook（COM vtable フック）で捕まえる
- ホイール対応（DirectInput の加工）が同じ DLL で完結する
- `plugins\*.asi` のロードにも対応し、他MODと共存できるようにする

### 想定スタック（導入済み）
- 言語: C / C++（32bit ターゲット）
- ビルド: CMake + MSVC（VS Build Tools 2026 / MSVC 14.51 / Windows SDK 10.0.26100）
- フック: MinHook（`third_party/` に取り込み予定）
- 解析スクリプト: Python 3.13（`.venv`、`tools/requirements.txt`: pefile, capstone）
- 静的解析自動化: Ghidra headless + JDK 21（Temurin 導入済み、Ghidra は未導入）
- リポジトリ: git（`main`）。ディスクの中身・インストール後のフォルダ・exe/アセット類は `.gitignore` で除外済み

### 決定4: ディレクトリとパス

| 場所 | 用途 | git |
|---|---|---|
| `D:\claudeCode\STCC\` | リポジトリ | ○ |
| `D:\Games\STCC\` | ゲームのインストール先（v1.02 適用、dgVoodoo2 配置） | × |
| `D:\Games\STCC_work\` | 解析用コピー（`orig100\`, `patch102\`） | × |
| リポジトリ内 `inventory/` | 棚卸し出力 | × |

---

## 4. 機能別メモ

### 0. Windows 11 対応で出やすい問題（DLL 側で対処）
1. **フレームレート依存**: 描画1回=1ステップや `timeGetTime` 精度前提だと、高リフレッシュレートで速度が狂う → DLL にフレームリミッタ
2. **マルチコア**: タイミング不安定 → 必要なら CPU アフィニティ固定
3. **高DPI**: MFC のメニュー付きウィンドウがぼやける・座標ずれ → DPI aware 宣言
4. **Alt+Tab / モード切替**: DirectDraw 排他フルスクリーンの復帰 → dgVoodoo2 設定と DLL で対処
5. **CD 依存**: ディスクチェック（`GetDriveTypeA`/`GetVolumeInformationA`）と CD-DA（`mciSendCommandA`）。当面は ImgDrive マウントで回避。最終的には DLL で `mciSendCommandA` をフックして FLAC 再生（dr_flac 等）し、`_inmm.dll` と仮想ドライブを不要にする

### A. ホイール対応

v1.02 は DirectInput 5 世代（`IDirectInputDevice2A`）で、**FFB 実装が既に存在する**（`GUID_ConstantForce`、SideWinder FF Pro 対応文字列）。

想定される罠と方針:
- **ペダル軸の合成/分離**: 当時は1軸合成、現代ホイールは分離 → **DLL 内で合成軸を作る**（vJoy / Joystick Gremlin 不要にする）
- **回転角**: 当時は200度前後想定 → DLL でスケーリング
- **XInput パッド**: DirectInput からはトリガーが1軸合成に見える → 同じ軸加工機構で対応
- FFB: 既存実装が現代ホイールで動くか確認。弱ければメモリから車速・横G等を読んで自前エフェクト

### B. ウルトラワイドスクリーン化

**exe 内に float の 4/3・3/4・640・480 定数が一切無い。** Saturn 版コード由来のため固定小数点演算の可能性が高い。Cheat Engine で `1.3333333` を探す定石はそのまま効かない前提で、16.16 固定小数点（4/3 = `0x00015555`、3/4 = `0x0000C000`）や、実行時に解像度から計算される値として追う。

21:9 / 32:9 で初めて出る問題:
- **HUD**: スクリーン座標ベタ書きのはず。左右端に飛ぶので座標変換が要る
- **カリング**（本命の落とし穴）: 4:3 視錐台前提で切っているはず。**カリング用の錐台パラメータ**を別に探す
- **描画距離 / LOD**: 同上

広げ方は **Hor+**。

### C. リプレイの自由カメラ

リプレイ機構が既存なので、やることは「毎フレーム、ビュー行列（またはカメラ位置＋注視点）を自分の値で上書きする」だけ。

手順:
1. リプレイ中にカメラ視点を切り替え、その瞬間に変化するメモリを差分検索で追う（Claude 製のメモリ差分ツールで行う。§7）
2. ビュー行列、またはカメラ構造体を特定する
3. DLL からフックして書き換える

`.reloc` が無く、ゲーム状態は固定アドレスの静的領域（.data 約14.5MB）にある可能性が高いので、特定したアドレスはそのまま使える。

覚悟しておくこと: カメラを大きく振るとスカイボックスの継ぎ目や、貼られていない背景が見える。

### D. 再現テスト

リプレイ `.DAT` を固定の検証シーンとして使い、同じフレームのスクリーンショットを比較して、ワイド化・カリング・HUD の修正前後を客観評価する。

---

## 5. 現在地と次のアクション

### 完了
- [x] Windows版ディスクを入手、bin+cue 化して F: にマウント
- [x] 開発環境導入（VS Build Tools 2026 C++、Python 3.13 + venv、JDK 21）
- [x] git 初期化、`.gitignore` 整備、ディレクトリ構成作成
- [x] ディスクの棚卸し（`tools/stcc-inventory.ps1` を改良して実行 → `docs/inventory.md`）
- [x] v1.02 パッチを `D:\Games\STCC_work\patch102\` に展開
- [x] v1.00 / v1.02 の exe 比較（`tools/exe_probe.py`）、版ハッシュを `data/addresses/versions.toml` に登録

### 棚卸しで出た答え
1. **DirectX 世代**: v1.00 は DirectDraw のみ、**v1.02 は DirectDraw + Direct3D IM（DX5, `IDirect3D2`）**
2. **入力**: **DirectInput 5**（`DirectInputCreateA`, `IDirectInputDevice2A`）、FFB あり
3. **アーカイブ**: 巨大1ファイルではなく、`Data\Bin\*.BIN`（242個）、`*.MRG`（25個）等の個別ファイル
4. **セットアップ**: **32bit**。Win98 VM は必須ではない（比較が必要になったときだけ構築）

- [x] Ghidra 12.1.3 導入（`D:\Tools\ghidra_12.1.3_PUBLIC`）、headless 解析（プロジェクト `D:\Games\STCC_work\ghidra\STCC`）、`tools/ghidra/` 整備
- [x] 主要 import の呼び出し元と DDraw/DInput/MCI 初期化を逆コンパイル → `data/addresses/stcc_jp-1.02.toml`
- [x] `D:\Games\STCC` へインストール（Win11 で `Setup.exe` 直接実行、DirectX なし）、差分ゼロ確認、設定は `C:\WINDOWS\stcc.ini`
- [x] v1.02 適用（exe 手動上書き、ハッシュ確認済み）

### 判明した重要事項（Phase 1〜2 途中）
- ゲームは**ウィンドウモードを内蔵**（`g_bFullscreen`）。解像度表は 640x480 / 320x240 × 16bpp / 8bpp の4件のみ（`g_ModeTable @ 0x4BD530`）
- DirectInput は `DINPUT_VERSION 0x500`、ジョイスティックのみ列挙、最大2台
- **dgVoodoo2 v2.87.4 の zip が Windows Defender に `Trojan:Win32/Kepavll!rfn` として検出・ブロックされた**（2026-09-13）。誤検知の可能性は高いが未確認。Defender の除外設定はユーザー判断事項。代替候補は DDrawCompat

- 上記 dgVoodoo2 zip はユーザーが外部サービスでスキャンしてクリーンと判断し、復元済み（SHA-256 `74AEB464…DEA0956E`、GitHub 配布物とサイズ一致）。`D:\Games\STCC_work\dgVoodoo2\` に展開済み
- 起動時に Windows の「DirectPlay が必要」ダイアログが出る（exe が `DPLAYX.dll` を静的 import しているため）→ Windows の機能として DirectPlay を有効化する

### 次にやること
- [ ] ラッパーなしでの基準起動テスト（起動可否・表示・Graphics メニュー・BGM・速度）
- [ ] `tools\wrapper.ps1 enable` で dgVoodoo2（`tools\dgvoodoo\dgVoodoo.conf`）を配置して同じ項目を比較
- [ ] Win11 での不具合を一覧化（§4-0 のチェック）、基準状態をバックアップ
- [ ] **Phase 3**: `dinput.dll` プロキシの骨格（CMake、MinHook、ini、ログ、版判定、DDraw/D3D/DInput/MCI 呼び出しログ）
- [ ] `tools/run.ps1`（ビルド → 配置 → 起動 → ログ回収）

### マイルストーン
1. 現代Windowsでとにかく起動する（dgVoodoo2、CD-DA、解像度）
2. ホイールでまともに走れる
3. 16:9 で破綻なく表示される
4. 21:9 でカリング・HUDまで含めて破綻しない
5. リプレイでカメラが自由に動く

---

## 6. 制約・作法

- **ディスクイメージ、アセット、改変済みexeは配布しない。** 利用者は各自で原盤を用意する形式（ScummVM や devilutionX と同じ方式）
- 配布するとすれば自作のラッパーDLLとツール類のソースのみ（公開時のライセンスは MIT を想定。MinHook は BSD-2）
- 実在ライセンス車両（Alfa Romeo、Toyota、Mercedes-AMG、Opel）を使っている点が、セガが今も再販できていない理由とみられる。この作品では特に上記を厳守する
- 原盤ディスクは保管し、以後の作業は吸い出したイメージに対して行う
- BGM は CD-DA（Track 2〜19）。**BIN+CUE、または データトラック＋各音楽トラックをFLAC** で保持すること

---

## 7. ツールと分担

| 用途 | ツール | 担当 |
|---|---|---|
| 静的解析（下調べ・検索・逆コンパイル抽出） | Ghidra headless | **Claude Code** |
| 静的解析（GUI での確認） | Ghidra | 人間 |
| メモリ差分検索 | Python（`ReadProcessMemory`）/ DLL 内ダンプ機能 | **Claude Code**（人間は操作とキー押下だけ） |
| 動的解析（デバッガ） | x64dbg | 人間 |
| D3D/DDraw ラッピング | dgVoodoo2 | 人間（配置）/ Claude（設定案） |
| 旧OS環境（必要時のみ） | 86Box または VirtualBox + Win98 | 人間 |
| ビルド・配置・起動・ログ回収 | `tools/run.ps1` | **Claude Code** |
| スクリプト・パーサ・ビューア作成 | Python / C++ | **Claude Code** |
| ラッパーDLL実装・ビルド | C/C++ + CMake + MSVC | **Claude Code** |
| バイナリの機械的解析 | Python (pefile, capstone) | **Claude Code** |
| 知見の記録 | `data/addresses/*.toml`（DLL ヘッダ生成・Ghidra ラベル入出力の元）、`docs/` | Claude Code |

Ghidra プロジェクトは git 管理しない。**判明したアドレスと意味は必ず `data/addresses/stcc_jp-1.02.toml` に書き出す。**

### 参考にする先行事例
- **Wanszai** の Virtua Racing / Sega Rally（Model 2）向け移植フレームワーク
- **Sega Rally 2** のコミュニティ製互換性修正パッケージ（dgVoodoo2、`_inmm.dll`、hex編集による1080p/ワイド化）
- **MAME の model2 ドライバのソース** — アーケード版の挙動確認用

---

## 8. セッション再開時のプロンプト例

```
このリポジトリの HANDOFF.md を読んでください。
STCC（Sega Touring Car Championship, Windows版1998）の
勝手移植プロジェクトです。

今日やりたいこと: <ここに書く>
```
