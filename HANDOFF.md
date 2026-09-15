# STCC 勝手移植プロジェクト — 引継ぎドキュメント

最終更新: 2026-09-13 夜（ホイール対応・枠なし全画面を別 PC TAROCOCKPIT で実施）
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

### 基準起動テスト結果（ラッパーなし、2026-09-13）
- 環境: モニタ3枚（主: 4K を 175% スケーリング＝論理 2194x1234、副: 同 4K、上: 1920x1080）、GPU 2枚（RTX 2070 / UHD 630）、デスクトップ 32bpp
- **起動はする。** タイトルバー付きの全画面表示
- **Alt+Tab でクラッシュ。** マルチモニタで解像度切替が乱れ、最終的に落ちる。表示以外（Graphics メニュー、BGM、速度）は検証不可
- クラッシュ: `STCC.EXE+0x47C44` の `0xC0000005`（→ `SwRast_DrawTexturedSpans @ 0x4479F0`、ソフトウェア描画のスパン書き込み。サーフェス喪失後の古いバッファへの書き込みと推定）
- Windows が自動で互換シム **`DWM8And16BitMitigation`** を付与（`HKCU\...\AppCompatFlags\Layers` の `D:\Games\STCC\STCC.EXE`）
- MFC の設定キー `HKCU\Software\SEGA\SEGA Touring Car Championship for PC\{Settings, Recent File List}` が作られるが値は空。`C:\WINDOWS\stcc.ini` への書き込み・VirtualStore 分岐・ゲームフォルダへの新規ファイルはなし
- **結論: 素の DirectDraw は Win11 マルチモニタ環境では実用不可 → ラッパー必須**

### dgVoodoo2 v2.87.4 でのテスト結果（2026-09-13）
設定: `tools\dgvoodoo\dgVoodoo.conf`（ウィンドウ化, stretched_ar, FPSLimit 60, SystemHookFlags=gdi, DesktopBitDepth=16）

| 構成 | 結果 |
|---|---|
| 原本 `STCC.EXE`（全画面）+ DirectDraw | 起動・BGM（CD-DA）・速度 OK、Alt+Tab で落ちない。**メニューバーは描画されない**（非クライアント領域は gdi フックの対象外）。F5 等のダイアログは表示・操作可能 |
| テスト exe（`testpatch.py windowed`、ウィンドウ）+ DirectDraw | **表示・BGM・速度 OK、メニューバー表示、Game→Exit で正常終了**（`DesktopBitDepth=16` 前は画面が緑一色） |
| テスト exe（ウィンドウ）+ Direct3D | 「Direct3Dモードでは起動できません。」→ `STCCD3D.DAT` に D3D 選択が保存され以後起動不能（ファイル退避で復旧） |
| 原本（全画面）+ Direct3D（F3 → Alt → D → Enter で盲目操作） | **切替成功、全画面でプレイ可能。** ただしメニューが見えず中断・終了できず強制終了 |

操作メモ: F3 = ポーズ/メニューモード切替（カーソル＋メニュー）、Esc = メニューモード解除、Alt+F4 = 終了、F5〜F9 = 各種設定ダイアログ

### Phase 3 進捗（2026-09-13）
- [x] `dinput.dll` プロキシ DLL の骨格（`src/stccfix/`、CMake Win32 静的CRT、`tools/build.ps1 -Deploy / -Remove`）
- [x] 版判定（PE ヘッダ TimeDateStamp + SizeOfImage、書き換え前に元バイト照合）、`stccfix.log` 出力
- [x] ウィンドウ起動パッチを DLL のメモリパッチ化（`stccfix.ini [Display] Windowed=1`）。**原本 exe + DLL で、ウィンドウ・メニューバー・表示・BGM・速度・正常終了すべて OK を確認**。テスト exe は `D:\Games\STCC_work\testexe\` に退避
- ログで `DirectInputCreateA(version=0x500)` が起動中に2回呼ばれることを確認（同じインターフェースポインタ）

- [x] MinHook v1.3.4 導入（`third_party/minhook` サブモジュール）
- [x] DirectDraw/Direct3D 呼び出しログ（`hooks_ddraw.cpp`、vtable 差し替え）とゲーム側 D3D 初期化関数の戻り値ログ（`hooks_game.cpp`、naked デトア）。`stccfix.ini [Debug] LogGraphics`
- [x] **ウィンドウ + Direct3D の失敗原因を特定・修正**: ウィンドウ時の描画先が SYSTEMMEMORY で作られ `CreateDevice(HAL)` が `D3DERR_SURFACENOTINVIDMEM`。`0x436548` を VIDEOMEMORY|3DDEVICE にパッチ（`[Display] D3DWindowedVideoMemory=1`）
- **ユーザー確認（2026-09-13）: ウィンドウ + Direct3D で起動・プレイ・終了・D3D のまま再起動すべて OK。** シーン切り替え時に軽いラグ感あり
  - ログ解析: シーン切替で約450枚のテクスチャを数秒で一括作成、さらに **`IDirect3D2::CreateViewport` が毎フレーム（80〜110回/秒）呼ばれる**（ゲームが viewport を毎フレーム作り直している）
  - ログを毎行 `FlushFileBuffers` していたのがラグ要因と推定 → フラッシュ廃止、CreateViewport ログは最初の3回と失敗時のみに
  - 毎フレームの viewport 再生成自体も将来の最適化候補（DLL で使い回す）

### マイルストーン1 状況
**達成**: 原本 exe + `dinput.dll`(stccfix) + dgVoodoo2 で、Win11 マルチモニタ環境でウィンドウ表示・メニュー・DirectDraw/Direct3D 切替・BGM(CD-DA)・正常終了が動く。
残課題: シーン切替のラグ、CD 不要化（MCI フック）、stcc.ini の VirtualStore 対策、フレームリミッタを DLL 側へ

- **ラグ解消をユーザー確認（2026-09-13）**。原因はログの毎行フラッシュだった

### 進め方の合意（2026-09-13）
1. **Phase 4: 入力** — 開発機のホイールは別PCのため、**Bluetooth 接続の PS4 互換コントローラ**を対象に DirectInput の軸加工基盤を作る（L2/R2 → 合成ペダル軸、スティックのデッドゾーン・カーブ）。ホイールは同じ基盤で後から対応
2. **Phase 5a: 高解像度化** — dgVoodoo2 の `Resolution` 強制で先に安定させる
3. **Phase 5b: ワイド化** — 投影・カリング・HUD の解析（§4-B）

### Phase 4 調査結果（2026-09-13, DS4 v2 Bluetooth, VID 054C PID 09CC）
- DirectInput 5 では `devType=0x10404`（JOYSTICK / サブタイプ GAMEPAD）、軸6・ボタン14・POV1、FFB なし。DS4 は既知名表に無いので汎用扱い
- ゲームは起動時にデバイス作成・レンジ設定・Acquire まで行うが、**既定（Keyboard 選択）では `GetDeviceState` を一度も呼ばない**
- 入力の選択は F5「Device Settings」（DIALOG 102/190）: Player 1/2 ごとに Keyboard / Game Pad / Joystick / SideWinder Game Pad / SideWinder 3D Pro Type / Steering Wheel（T2）/ Per4mer Racing Wheel、各「Next」で割り当てダイアログ（Game Pad=114, 設定=115/116, 調整=118/121）
  - 116 には「アクセルペダル」「ブレーキペダル」、121 には「アクセルとブレーキはスロットルかスティックを割り当てないと調整出来ません」→ 軸割り当て式
- テスト中に SHARE か OPTIONS 押下で BT 切断が1回発生（原因未確認）
- **F5 → Player 1「Game Pad」で動作を確認**: 左スティックでステアリング、□ = Button1（アクセル）、× = Button2（ブレーキ）。アクセル/ブレーキはデジタルのみ
- **BT 切断が頻発**: System ログに `HidBth` イベント 2（went out of range or became unresponsive）が 90 分で 9 回、13:24〜13:26 は約 25 秒ごと。ゲーム起動前にも 1 回。BT リンク層の問題で、ゲームが原因ではない。互換（非純正）DS4 + Steam 起動中（Steam の PS4 対応による拡張モード切替が疑わしい）
- 切断後のゲーム挙動: `GetDeviceState` → `DIERR_INPUTLOST`、以後 `Acquire` が毎フレーム `DIERR_UNPLUGGED`(0x80040209)。**ゲームは再列挙しないので、再接続しても F5 を開き直すまで入力が戻らない** → DLL で自動再接続する候補
- 切り分け用に `tools/joylog`（DirectInput8 非排他、切断/再接続・Steam 起動有無を記録）を作成
- **joylog 結果（2026-09-13 14:31〜14:39、ゲーム非起動）: 切断 0 回**（Steam 起動中 約4.5分 / Steam 終了後 約3分とも）、同時間帯の HidBth イベントも無し → **Steam は原因ではない。切断はゲーム実行中にだけ起きている**（排他 Acquire やゲーム中の負荷など、要切り分け）
- **DS4（互換品）の DirectInput 割り当て（確定）**
  | 入力 | DirectInput |
  |---|---|
  | 左スティック 左右 / 上下 | X / Y（0..65535、上が 0） |
  | 右スティック 左右 / 上下 | Z / Rz |
  | L2 / R2 アナログ | Rx / Ry（0..65535） |
  | 十字キー | POV0（上 0, 右 9000, 下 18000, 左 27000） |
  | □ × ○ △ | b0 b1 b2 b3 |
  | L1 R1 / L2 R2(デジタル) | b4 b5 / b6 b7 |
  | SHARE OPTIONS / L3 R3 / PS タッチパッド | b8 b9 / b10 b11 / b12 b13 |
  - ゲームの Game Pad 設定の Button1 = b0(□)、Button2 = b1(×)
- **F5 Device Settings で DS4 を繋ぐと、選択できるのは Keyboard と Game Pad のみ**（Joystick / SideWinder 系 / Steering Wheel（T2）/ Per4mer はグレーアウト）→ ゲームがデバイス種別（DIDEVTYPE サブタイプ=GAMEPAD）や既知名で選択肢を絞っている。**DLL で種別を JOYSTICK / WHEEL に見せればアナログモードを解放できる見込み**
- **DLL でサブタイプを偽装（`[Input] DeviceType`）→ Joystick / Steering Wheel（T2）が選択可能に**。どちらも実機で「左スティック左右 = ステアリング、上下 = アクセル/ブレーキ（Y 軸 1 本）」、他のボタンは無効（2026-09-13）
  - Joystick の設定タブ: アクセル = Stick Up、ブレーキ = Stick Down、シフト/決定/キャンセル/視点/スタートはボタン割当。調整タブ: ハンドル/アクセル/ブレーキの最大値と効き始め
  - Steering Wheel の設定タブ: シフトアップ/ダウン = Down/Up、視点変更 Button1、スタート Button2、「アクセル及び決定 = アクセルペダル」「ブレーキ及びキャンセル = ブレーキペダル」
  - → `[Input] TriggerPedals`: R2(Ry)/L2(Rx) から Y を合成する軸加工を実装（両方離していればスティックの Y をそのまま通す）。ステアリングのデッドゾーン/カーブも追加
  - **ユーザー確認（2026-09-13）: Joystick モードで R2 アナログアクセル・L2 アナログブレーキ・スティック上下・ステアリングすべて OK**。調整タブでの最大値/効き始め設定も可能な見込み
- **マイルストーン2（コントローラ版）達成**: DS4 でアナログ操作できる。ホイールは同じ基盤（AccelAxis/BrakeAxis/Invert）で後日対応
- Phase 4 残課題: 切断時の自動再接続（ゲームは再列挙しない）、Steam 起動中の切断対策の確認（Steam 設定の PlayStation 対応 OFF で解消するか）、既定ログを LogInput=0 に
- **Steam 終了状態でゲーム中に切断は起きなかった**（2026-09-13 午後）。切断はこれまで全て「STCC 実行中 かつ Steam 起動中」→ ゲームの排他 Acquire と Steam の PS4 対応の競合が有力。当面 Steam は終了してテストする

### Phase 5a 高解像度化（2026-09-13）
- dgVoodoo2 `Resolution` 強制で Direct3D は明確に高精細化。DirectDraw はソフトウェア描画の拡大なのでモザイク状（D3D 推奨）
- **ゲームはメニューを 640x480、レースを Settings > Screen Mode の解像度（今回 320x240）で描く**。モード切替のたびに `0x436750` から `SetWindowPos` で窓をそのクライアントサイズへ戻す
- stccfix `hooks_window.cpp`: 起動時に窓を自動拡大（`WindowScale=0` = 作業領域に収まる最大整数倍、4K で 2560x1920）、ドラッグ時 4:3 拘束、利用者サイズの記憶、`WM_WINDOWPOSCHANGING` で 640x480 / 320x240 要求を差し替え＋300ms 後に 1px ナッジで WM_SIZE を再通知。`[Debug] LogWindow` で SetWindowPos/MoveWindow の呼び出し元を記録
- dgVoodoo `Resolution` は倍率（4x）だと 320x240 のレースが 1280x960 止まりになるため、固定 `2560x1920` に変更

- **ユーザー確認（2026-09-13）: レース開始でも窓が大きいまま維持。Screen Mode 640x480 のほうが 320x240 より明らかに綺麗** → 推奨設定は Direct3D + Screen Mode 640x480 16bit
- **マイルストーン「4K で見やすい表示」達成**。残る見た目の課題: HUD・文字などの 2D スプライトは 640x480 素材の拡大なのでドットが粗い（素材由来、ワイド化時の HUD 配置と合わせて扱う）

### Phase 5b ワイド化 調査結果（2026-09-13）
- **ゲームは 3D も 2D も、自前で 640x480 画面座標へ投影した `D3DVT_TLVERTEX` を `IDirect3DDevice2::DrawPrimitive` で渡すだけ**（レース中 1.2〜2.5 万回/秒、4 頂点ずつ。SetTransform 0 回、sz は常に 0 = Z バッファ不使用、rhw 0.003〜2.67）。画面外にはみ出す大ポリゴンは x が ±数万に達し D3D がクリップ
- viewport は毎フレーム作り直し、`SetViewport2` は 640x480 clip[-1, 0.75, 2, 1.5] と 639x479 の 2 種を交互（TL 頂点には影響しない）
- exe に 320/240/160/120 の float/double 定数なし。画面サイズ変数 0x567A10/14 を読むのは DDraw 初期化と窓リサイズのみ → 投影パラメータは別に保持
- IDirect3DDevice2 の vtable 順に注意（GetCaps の後に SwapTextureHandles/GetStats/AddViewport/DeleteViewport/NextViewport）。取り違えで起動不能になった
- **方針: アナモルフィック方式**。DLL が TL 頂点の x を画面中心から (4/3)/(W/H) 倍に縮めたコピーを渡し、窓を W:H にして dgVoodoo に横へ引き伸ばさせる（`[Display] AspectRatio`、dgVoodoo `Resolution` も同比率に）。残課題: 4:3 前提のカリングによる画面端の湧き、HUD の配置、全画面背景

- **ユーザー確認（2026-09-13）: 16:9 で全体に違和感なし**。空（x が 0..640 ぴったりの全幅ポリゴン）が中央 75% に縮んで左右が黒抜け → 全幅ポリゴンは縮めず `WideBackground=extend`（u を 1/k 倍に拡張）で解消を確認
- HUD（Qualifying、タイム、速度計）は画面端に出たまま横に伸びている → TL 頂点（DrawPrimitive）ではない
  - ログ: Blt は毎フレーム 1 回だけ（転送先 = 窓クライアント全体 = 表示用の転送）、BltFast 0 回 → HUD は Blt でもない
  - 残る候補は描画先サーフェスを Lock してピクセルを直接書くソフトウェアスプライト描画（Saturn VDP1 相当）。Lock 呼び出し元の集計で特定する
  - **Lock 集計: レース中、640x480 描画先は毎フレーム 1 回、戻りアドレス `0x436808` からだけロックされる**（`0x440B97` は 8x8〜64x64 のテクスチャ転送）→ HUD はこのロック中に書き込まれていると推定
  - ロック補助 `Gfx_LockRenderSurface 0x4367C0` / `Gfx_UnlockRenderSurface 0x436890` の 7 組すべてで、間に `Spr_RenderQueue 0x46D020(desc)` を呼ぶ。スプライト登録は `Spr_Enqueue 0x421D10({x,y,w,h,0,0,pattern,frame,flags})`（640x480 座標）
  - → HUD 補正はスプライトのキュー（x, w）を横に k 倍する方針で検討中（端寄せ or 中央寄せ）
- 軽微（ユーザー判断で当面許容）: コース沿いの観客席などが画面端で出現するのが少し気になる（4:3 前提のカリング/LOD）、路面の継ぎ目がわずかに見える

- **方式変更（2026-09-13, ユーザー合意）: アナモルフィック → 横長描画面方式**
  - Direct3D の描画先（3DDEVICE・非主画面、640x480/320x240）を表示比率の幅で作成（16:9 = 854x480、offset 107）
  - `Lock`: ゲームには中央 4:3 部分（lpSurface + offset、dwWidth 640）を渡す → **HUD/2D は伸びずに中央 4:3 に表示**。3D を描かないフレームは左右余白を黒で消去
  - `DrawPrimitive`: TL の x を +offset（縮めない）。全幅背景は描画面の端から端へ + u 拡張
  - `SetViewport(2)` / `Clear`: 全幅を広げる。`Blt`: 表示転送は広げた幅ごと、描画面への Blt は +offset。`BltFast` は x+offset
  - **ユーザー確認（スクリーンショット 2 枚）: HUD は伸びずに中央 4:3 領域へ、3D は画面全幅、空も自然、他車も画面右端まで描画**

- **32:9 もユーザー確認（2026-09-13, 4K 画面で窓 3413x960、描画面 1706x480、dgVoodoo 3413x960）**: タイトル/メニューは中央 4:3 で左右は黒（ゴミなし）、レースは全幅に 3D、HUD は伸びずに中央。速度も問題なし
  - 比率の切り替えは `tools\aspect.ps1 <W:H> [-RenderHeight N]`（配置済み stccfix.ini の AspectRatio と dgVoodoo.conf の Resolution を同時に変更）

- **HUD 端寄せ（`hooks_hud.cpp`、2026-09-13）**: Spr_RenderQueue(0x46D020) をフックし、3D を描いたフレームでスプライトを塊ごとに左端/中央/右端へ移動。描画関数は lpSurface + x*bpp + y*lPitch で書き、クリップは g_SprScreenW/H(0x10E4C40/44) だけを見るので、広い面の先頭と広い画面幅を一時的に渡す。`[Display] HudAnchor=edges|center`
  - ユーザー確認（32:9）: PC モード・アーケードモードとも Qualifying 左端、COURSE RECORD/速度計/AT 右端で良好。ただしコースレコードの数字が大きなラップタイムと上下 1px 接して同じ塊になり中央に残った → 塊は「同じ行（縦に半分以上重なる）で横に近接」に変更
  - 車選択（回転する 3D 車あり）とレース前のグリッド画面で一部（TRANSMISSION、NEXT Round 等）が右端へ → レース画面フラグ 0x56C190 も条件に追加（グリッド画面は残る可能性、優先度低）

  - ユーザー確認（改善版）: レース中のコースレコード数字も右端へ移動、車選択は中央に戻り良好。**レース前のグリッド画面は行ごとに左右へ散って崩れた** → 画面幅いっぱいのスプライト（区切り線・帯）があるフレームは「レイアウト画面」とみなし全体を中央のままにする

  - **ユーザー確認: グリッド画面も中央表示に戻った**（2026-09-13）。HUD 端寄せはレース中のみ・メニューとレイアウト画面は中央、で完成

### 次にやること: ステアリングホイール対応（別 PC）
- 対象: **Fanatec CSL DD Pro（ホイールベース）+ ペダル、シフターなし**。ホイールは開発機とは別の PC に接続されているので、そちらへ環境を移して作業する
- 手順案
  1. `tools\joylog` でデバイス名・軸・ボタンの割り当てを記録（ペダルがベース経由か USB 単体か、アクセル/ブレーキの軸と向き）
  2. ゲームの Device Settings で出る選択肢の確認（ホイールとして列挙されれば Steering Wheel(T2)、されなければ `[Input] DeviceType=wheel`）
  3. `[Input] TriggerPedals=1` + `AccelAxis` / `BrakeAxis` / `AccelInvert` / `BrakeInvert` でペダルを Y 軸へ合成（DS4 用に作った仕組みをそのまま使う）
  4. 回転角: 当時は 200 度前後の想定。Fanatec 側の SEN（回転角）設定で絞るか、DLL にステアリング倍率（SteerGain）を追加
  5. FFB: ゲームは既知機種（SideWinder FF Pro 等、種別 3/8）にしか ConstantForce を作らない。DD ベースで手応えを出すには DLL 側で FFB を実装（§4-A のテレメトリ方式）。安全のため強さは控えめから

### ホイール PC（TAROCOCKPIT）での作業結果（2026-09-13 夜）
環境: モニタ 1 枚 5120x1440（32:9、125% スケーリング）、RTX 3060、FANATEC driver 0.53.1 + FanatecApp、互換 DS4 も USB/BT で接続
- セットアップ: リポジトリと `D:\Games` はフォルダごとコピー済みだった。VS Build Tools 2026（18.10, MSVC 14.51, SDK 26100）を winget で導入、`.venv` は Python 3.14.2 で作り直し（pefile/capstone OK）、コピーされた `build\CMakeCache.txt` は削除して再構成
- **マイルストーン2（ホイールでまともに走れる）達成**: CSL DD + ペダルで Steering Wheel (T2) モード、ステア・アクセル・ブレーキ OK（ユーザー確認）
- 分かったこと
  - DirectInput では "FANATEC Wheel" が 2 台（HID コレクション 2 つ）見え、2 台目は値が動かない。DS4 も同時に見える → `[Input] DeviceName=FANATEC#1` で 1 台だけゲームに見せる
  - **ゲーム（DirectInput 5）の軸: X=ステア, Y=クラッチ, Z=アクセル, Rz=ブレーキ**（すべて離して最大）。joylog（DirectInput 8）では Y=アクセル に見えて食い違う → 軸は必ず `LogInput=1` の stccfix.log で決める
  - 設定: `DeviceType=auto`（ネイティブで sub=6 WHEEL → T2 が選べる）, `TriggerPedals=1`, `AccelAxis=Z`, `BrakeAxis=Rz`, `AccelInvert=1`, `BrakeInvert=1`, `PedalDeadzone=2`
  - ホイールでは Y の素通しをしない（Y=クラッチ離し=最大がブレーキ全開になっていた）。GetCapabilities の本来のサブタイプで判定
  - 回転角: FanatecApp の SEN 1080 ではゲーム入力 48..208 のうち 123..151 しか使わない → **SEN 240 前後**で良好
  - FFB: ゲームは T2 でも AUTOCENTER と ConstantForce を作るが、強さを更新するのは種別コード 3/8 だけで、他は初期値のエフェクトを Start するだけ（スタート時の変な力）→ `[Input] ForceFeedback=0` で DIDC_FORCEFEEDBACK を隠す。関数は toml に記録（FF_SetConstantForceA/B, FF_StartEffect 等）
- ノーズ視点のバックミラー: 画面より小さい viewport が 4:3 位置のままで TL 頂点だけずれていた → 部分 viewport も offset（ユーザー確認で完璧）
- **枠なし全画面** `[Display] BorderlessFullscreen=1` + `DpiAware=1`、dgVoodoo `Resolution=5120x1440`（`tools\aspect.ps1 32:9 -RenderHeight 1440`）
  - DPI 非対応のままだと窓が 4096x1152 の仮想座標になる → DllMain で Per-Monitor V2 を宣言
  - dgVoodoo は SetCooperativeLevel 時点の窓の形で表示先を決める → 枠外しは Gfx_InitDirectDraw の**前**に行う。ゲームの 640x480 要求はモニタ全体に固定、MFC の SetMenu（0x43C530 から）は保留
  - **実行中の切り替えは不可**: Alt+Enter を試したが、窓→全画面で広げると dgVoodoo の表示が元の大きさのまま（縮めるのは追従）→ 起動時のみ。メニューが要るときは ini で 0
  - 自動テスト: スクラッチの PowerShell（起動→CopyFromScreen→keybd_event→終了）で確認できた。今後も表示系の確認に使える
- 残課題（ホイール）: FFB の自前実装（テレメトリ方式）、Fanatec の SEN をゲーム別プロファイルにするか DLL に SteerGain を持つか

### FFB の試み（2026-09-13 夜、未完成・既定 off）
- `[ForceFeedback] Mode=off|native|game`（既定 off）。game は FF 対応デバイスを "DIforce2 Serial Joystick Device"（種別コード 8）に見せる。コード 3（SideWinder FF Pro）は F5 で「SideWinder 3D Pro Type」＝ジョイスティック扱いになり、保存済みの T2 と合わずホイールが読まれなくなった。コード 8 は「Per4mer Racing Wheel」で選べる
- ゲームの力: 整数値 ×40000、不定期（毎秒 2〜30 回、数秒来ないことも）、元は 30ms の単発エフェクト。走行中の値は低速 1〜10、250km/h 超のカーブで 50〜147、予選の停止状態から約 −49
- stccfix の加工: 目標値として受け、GetDeviceState から毎フレーム送る（無限長の ConstantForce 1 本）。units/100 → Curve → Gain → MaxForce、HoldMs で解放、Smoothing で変化率制限、途切れ後 FadeInMs
- ユーザー評価: 当初は 0/最大の二極化と衝撃の残留（尺度 100 倍ずれ・無限長）→ 修正後も「ほぼ衝突時だけ」「予選開始の衝撃は変わらず」→ **いったん off で打ち切り**。次はメモリから車速・横 G・ステア角を読むテレメトリ方式で作り直す想定

### 公開準備（2026-09-13 夜）
- 公開名 **stcc-pc-tweak**（GitHub taro-youngdisciples/stcc-pc-tweak）、MIT（taro-youngdisciples）、履歴ごと公開、プレリリース v0.1.0、当面インタラクション制限（collaborators_only）
- コミットの作者メールは公開前に GitHub noreply へ書き換える（ローカル git 設定も noreply）
- 説明書: README.md（英）/ README.ja.md / docs/DEVELOPMENT.md / LICENSE / THIRD_PARTY_NOTICES.md。リリース zip は `tools\package.ps1 -Version x.y.z`（dll, ini, dgVoodoo.conf, README×2, LICENSE, NOTICES のみ）
- **元にしたディスクは日本版の再販「Ultra2000 シリーズ」**（ユーザー情報。ボリュームラベル `Touring_Car`、フォルダ日付 2001-04-18、`D3D\updatej.exe` 2001-04-08 入り）。1998 年の初回版はパッチが別配布の可能性、海外版は exe 別物で非対応 → README に明記
- https://github.com/taro-youngdisciples/stcc-pc-tweak を非公開で作成しユーザー確認後、**2026-09-13 に公開（public）**。プレリリース v0.1.0（zip 添付、タグは README 追記後のコミットに付け直し済み）
- **インタラクション制限: collaborators_only、期限 2027-03-13**（GitHub の上限 6 か月）。延長するなら期限前に `gh api -X PUT repos/taro-youngdisciples/stcc-pc-tweak/interaction-limits -f limit=collaborators_only -f expiry=six_months`
- 次の候補: 60fps 化（28 FPS 上限の原因調査: タイマー精度 / 1 描画 2 ステップ / 30Hz ロジック）、テレメトリ方式の FFB

### 60fps 化の調査（2026-09-14、静的解析のみ。詳細は stcc_jp-1.02.toml の「フレームの流れとタイミング」）
- このPCには Ghidra/JDK が無い（プロジェクトファイルだけコピー済み）→ capstone で解析
- **フレームの流れ**: App_Run(0x439A00, CWinApp::Run) が待たずに Game_Frame(0x417400) を回す。1 回 = 入力 → 状態関数 1 回 → 描画 → 上限
- **28 FPS の原因**: Frame_Limit30(0x417130) が GetTickCount で「3 フレーム 100ms」までビジーウェイト。GetTickCount の 15.6ms 刻みで実測約 28 FPS・不安定。有効条件は g_FrameFlags(0x10F61A0) bit0
- **設計は 30Hz 固定ステップ**: 全状態関数が毎フレーム FrameSkip_SetTarget(33ms) を呼び、遅れると描画だけ省く（FrameSkip 0x7F5210）。レース本体 Race_UpdateRunning(0x41B410) の物理は車ごとに 1 ステップ 1 回で、サブステップなし。1/30・1/60 の浮動小数点定数も無い（固定小数点で 1 ステップ分ずつ）
- → 上限を外すと**ロジックごと倍速**になる見込み。今の 28 FPS はゲーム速度も約 93% に落ちている可能性（要実測）
- 方針候補
  1. **正確な 30.00 FPS**（Frame_Limit30 を QueryPerformanceCounter の待ちに差し替え）: 簡単。カクつきと速度低下を解消
  2. **補間による 60fps 表示**: ロジックは 30Hz のまま、ステップ間に中間の姿勢で 1 回余分に描く。車の位置・向き・カメラなど「描画に使う状態」の特定が必要。リプレイ自由カメラ（目標 3）と同じ知識が要るので相乗効果あり。難度 中〜高
  3. ロジックを 60Hz にして 1 ステップの量を半分: 定数が散らばっていて現実的でない
- 次の手順案: (a) DLL に計測（Game_Frame の実回数/秒、描画回数、スキップ回数）と上限差し替えの実験オプション → 実機でレースの速度と FPS を確認 (b) 1 を実装 (c) 2 の可否を、描画関数を 1 ステップ内で 2 回呼んでも状態が進まないかから調べる

### 60fps 化 (a) 計測結果（2026-09-14、`hooks_frame.cpp`: `[Debug] LogFrame` / `[Frame] TargetFps`）
- 自動計測（タイトル/アトラクトデモのみ、g_GameState=0。**レース中は未計測**）。各 45 秒、1 秒ごとの平均
  | TargetFps | ステップ/秒 | 描画 | 描画省略 | 速さ | 描画間隔の最大 |
  |---|---|---|---|---|---|
  | 0（ゲームの上限） | 約 34 | 27〜28 | 6〜7 | **約 113%** | 約 48ms |
  | 30（QPC＋高精度タイマー） | 約 34 | 30.0 | 約 4 | **約 113%** | 約 34ms |
  | 60 | 60 | 60 | 0 | **200%** | 約 17ms |
- **ロジックは描画フレームに連動（60 で倍速）を確認**。上限を変えるだけでは 60fps にできない
- **予想と逆で、本来より約 13% 速い**: 描画省略（FrameSkip）で上限を通らないステップが増える。省略判定は「直近 4 フレーム ≥ ms×4 = 132ms」で、正確な 30.00 FPS（133.3ms）でも基準を超えるため省略が止まらない → 数フレームに 1 回コマ飛び（PCGamingWiki の「フレーム間隔が不安定」の正体とみられる）
- 数秒おきに 1 秒だけステップが 50〜80 に跳ねる: 描画判定は 1 なのに上限が呼ばれない（Lock 失敗など、アトラクトの場面切替とみられる）
- 60 設定での待ちは 1 フレーム約 11ms → 描画は数 ms、性能に余裕あり
- 計測は切断/ロック中のセッションではできない（DirectDraw 初期化で STCC が即終了、終了コード 0）。本体モニタでサインインした状態で行う
- **(b) の設計**: 待ちを Frame_Limit30 ではなく Game_Frame の先頭で 1 ステップ 1/30 秒ごとに行い（元の上限は何もしない）、FrameSkip_ShouldRender を「締め切りから 1 周期以上遅れたときだけ省略（maxSkip は g_FrameSkip+8 を守る）」に置き換える。同じフレーム内の 2 回目の呼び出しには 1 回目と同じ値を返す。目標: ステップ 30・描画 30・省略 0・速さ 100%
- README の設定表への TargetFps / LogFrame の追記は、(b) で仕様が固まってから

### 60fps 化 (b) 正確な 30 FPS のペース配分（2026-09-14）
- `hooks_frame.cpp` を上記の設計で実装（Game_Frame 先頭で 1/N 秒待ち、4 周期以上遅れたら数え直し、元の上限は待たない、描画省略は 1 周期以上遅れたステップだけ・g_FrameSkip+8 の上限と +0x38 の停止要求は守る、2 回目の ShouldRender には同じ値）
- アトラクト（g_GameState=0）の自動計測 TargetFps=30: **ステップ 30.0・描画 30・遅れ 0・描画間隔 33.3ms で一定**（元は 34 ステップ・27〜28 描画・48ms）。数秒おきに 1 秒だけ省略約 10 回（late=0 なので +0x38 の停止要求 = 場面切替のゲーム仕様）、ロード時の空き 185ms 後はすぐ復帰
- 残り: レースでの確認（ユーザー）、問題なければリリース既定を TargetFps=30 に・README の設定表に追記、その後 (c) 補間の可否調査
- **ユーザー確認（2026-09-15）: 「早送り感がなくなり、ヌルヌルになった」** → リリース ini とキー未指定時の既定を TargetFps=30 に、README（機能・設定表・既知の制限）に反映
- 次: (c) 補間による 60fps 表示の可否調査

### 60fps 化 (c) 補間の可否調査（2026-09-15、`[Debug] LogDrawList`、drawlist.cpp）
- **3D は描画処理の中だけで描かれる**: DrawPrimitive の 100% が Frame_RenderA/B 内から。ポリゴン 1 枚ずつ Gfx_DrawQuadTL(0x440D40) で約 1000 回/フレーム、空の全画面四角形 Race_DrawSkyQuad(0x40E590) が 1 回/フレーム → ロジックと描画は分離している
- **ポリゴン一覧**: g_PolyListA/B (0x7D47C8/0x7D43E8)。積み込みの時点で投影済み（整数の画面座標 4 点）、PolyList_Flush(0x443A00) が並べ替えて 1 件ずつ描く。構造は toml の PolyList_*
- **方式 B（描画命令/一覧を前フレームと対応づけて補間）は不可**: アトラクトで、並べ替え後の DrawPrimitive 列の同位置一致 15〜30%（先頭一致約 1%）、並べ替え前（積み込み順）の材質一致 平均 8.7%（最低 3.8%）、件数一致 2%、一致しても平均数千 px 移動（別ポリゴン）。見える物が変わるたびに一覧がずれ、ポリゴンの同一性を取れない
- **残る方式 A（ゲーム状態の補間）**: 描画処理の直前にカメラと各物体（車・動く物）の位置/向きを前ステップとの中間に書き換え、描画して元に戻す。必要なのは (1) 描画処理が読む変換データ（カメラ、車の姿勢）の特定 (2) 描画処理の副作用（アニメーション/パーティクル/カウンタ）の確認。リプレイ自由カメラ（目標 3）と同じ解析なので相乗効果。難度は高め、要メモリ差分解析（ユーザーの操作が必要）
- 30fps のペース配分 (b) は完成していて、この調査とは独立
- **ユーザー判断（2026-09-15）: 60fps 化は 30fps の安定化で止める**。補間は外部ツール（Lossless Scaling など）でもよい

### 表示設定（F6 Game Settings）の調査（2026-09-15、詳細は toml「表示設定」）
- F6 は Direct3D なら DIALOG 193（Perspective Correction / Bilinear Filtering / Alpha Blending / Fog / Speedometer）、DirectDraw なら DIALOG 103（**走査方式 / Texture Detail** / Speedometer）
- **走査方式と Texture Detail はソフトウェア描画（DirectDraw）専用で、Direct3D では効かない**（Texture Detail はソフトウェアのポリゴン描画 0x7F5C60+0x14 だけが参照）
- TAROCOCKPIT の現在値: STCC.DAT で走査方式=ノンインターレース(0)、**Texture Detail=Low(1、ゲームの初期値)**、Speedometer=km/h。STCCD3D.DAT で Direct3D=ON、4 効果すべて ON
- Direct3D の 4 効果と SetRenderState の対応は確定（toml の g_D3DEffects）

### dgVoodoo の画質設定（2026-09-15）
- `[DirectX] Filtering = 16`（異方性）、`Mipmapping = autogen_bilinear`、`Antialiasing = 4x` を追加（リポジトリの conf と配置済み conf）
- アトラクトで同じ時点を等倍比較: **車・影・縁石のジャギーが明確に減り、遠くの観客席/フェンスのざらつき（走行中のちらつき）が落ち着く**。路面のくっきり感はほぼ同じ（元テクスチャが粗い）。HUD は崩れず、FPS も維持
- 比較用スクリプトはスクラッチの dgv_compare.ps1（conf を一時変更して起動・撮影・復元）。実レースでの確認はユーザー待ち

### 別 PC への移行チェックリスト
- リポジトリ: git（サブモジュール `third_party/minhook` を含む。`git clone --recursive` か `git submodule update --init`）。ゲームのファイル・exe・dgVoodoo 本体・棚卸し結果は .gitignore 済みでリポジトリに入っていない
- ツール: Git、VS Build Tools（C++ x86）、Python 3.13 + `.venv`（`tools\requirements.txt`）。Ghidra + JDK 21 は解析が必要になったときだけ
- ゲーム: ディスクイメージをマウントして `Setup.exe` → `D:\Games\STCC`（DirectX は入れない）→ v1.02 の `STCC.EXE` を上書き（ディスクの `D3D\updatej.exe` 内）→ DirectPlay の Windows 機能を有効化
  - LAN でインストール済みフォルダごとコピーする場合は、`C:\WINDOWS\stcc.ini`（DataPath/ExePath、要管理者権限）も同じ内容で置けば Setup は不要
- **ディスクイメージのマウントは必須**（2026-09-13 解析）: 起動時（0x439549）と 0x40B422 で `Cd_RequireDisc` が、DRIVE_CDROM 種別のドライブに `\stcc\stcc.exe` と `\stcc\data\bg\sky.bmp` があるかを確認し、無ければ「Please insert...CD」で止まる。ドライブ文字・ボリュームラベルは問わない。BGM も CD-DA（MCI cdaudio）なので、音楽トラック付き（BIN/CUE）でマウントする。将来 DLL で CD 不要化＋FLAC 再生にすれば不要になる
- dgVoodoo2 v2.87.4 を `D:\Games\STCC_work\dgVoodoo2` に展開（Defender が誤検知する場合あり）→ `tools\wrapper.ps1 enable`
- `tools\build.ps1 -Deploy` → `stccfix.ini` を配置 → `tools\aspect.ps1 <比率>`
- パス前提: `D:\Games\STCC`、`D:\Games\STCC_work`、`D:\Tools\ghidra_12.1.3_PUBLIC`（違う場合は各スクリプトの引数で指定）
- [ ] （任意）カリング幅の拡張、路面の継ぎ目
- [ ] Phase 4 残課題（自動再接続、Steam の PS4 対応 OFF 確認）
  - DirectDraw / Direct3D の COM 呼び出しログ → **ウィンドウ時に D3D 初期化のどこで失敗するか特定**（ゲーム側のエラー報告関数 0x42F290 は空）
  - `C:\WINDOWS\stcc.ini` 読み書きのリダイレクト（任意）
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
