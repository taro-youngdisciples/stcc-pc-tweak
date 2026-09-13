# ディスク棚卸し結果（Phase 1）

調査日: 2026-09-13
対象: 日本版ディスク（ImgDrive で F: にマウント、ボリュームラベル `Touring_Car`）
生データ: `inventory/disc.txt`, `inventory/disc.csv`（git 管理外。`tools/stcc-inventory.ps1` で再生成）

## 1. ディスク構成

| パス | 内容 |
|---|---|
| `Setup.exe` + `SSP.dll` / `SSP.INI` | 32bit インストーラ。`SourceCopy` で `Stcc\` をコピーし、レジストリ登録するだけの単純な構成 |
| `DSETUP*.DLL`, `DirectX\` | DirectX 5 ランタイム同梱。**Win11 では入れない**（`DSETUP16.DLL` は 16bit NE） |
| `IE40Jpn\` | IE4 同梱（オンラインヘルプ用）。不要 |
| `HELP\Japanese\` | HTML ヘルプ（日英） |
| `D3D\updatej.exe` | **v1.02 パッチ**（WinZip 自己解凍。中身は `STCC.EXE` / `PatchInstaller.exe` / `.ini` / `update.doc`） |
| `Stcc\STCC.EXE` | 本体 v1.00 |
| `Stcc\Data\Bin\` | 242 個の `.BIN`（コース `C1..C4*`、車両 `CAR2x*`、AI `AIS*` 等）＋ `.DAT` |
| `Stcc\Data\Resource\`, `Res_Jpn\` | `.MRG`（UI/2D リソースのアーカイブと推定） |
| `Stcc\Data\Bg\Mode16\` | 空の BMP（16bit カラーモード用） |
| `Stcc\Data\Wave\` | 効果音 WAV 59 個 |

CD トラック: 全 19 トラック。Track 1 = データ、**Track 2〜19 = CD-DA（BGM 18 曲）**。

インストーラ（`SSP.INI`）:
- 既定インストール先 `C:\SEGA\Touring Car`、ini 名 `stcc.INI`
- レジストリ: `HKLM\SOFTWARE\SEGA\...` に `ExePath` / `DataPath` / `InstallType`、および `App Paths` / `Uninstall`
- ゲーム側も `DataPath` / `ExePath` / `InstallType` を参照し、未挿入時は `Please insert The SEGA Touring Car Championship CD.`

## 2. 本体 exe

| 項目 | v1.00 | **v1.02（基準版）** |
|---|---|---|
| SHA-256 | `897E9AE4…17C6CA` | `6B4E92CA…C4319D` |
| サイズ | 946,688 | 1,009,152 |
| リンク日時 | 1998-01-12 | 1998-04-14 |
| ImageBase / .reloc | 0x400000 / **なし** | 0x400000 / **なし** |
| .data 仮想サイズ | 0xC954E8（約13MB） | 0xDD5508（約14.5MB） |
| DDRAW imports | `DirectDrawCreate` | `DirectDrawCreate`, `DirectDrawEnumerateA` |
| Direct3D | なし（ソフトウェア描画） | **`IID_IDirect3D2` / `IID_IDirect3DTexture2`（DirectX 5 世代 IM）** |
| 追加文字列 | — | `STCCD3D.DAT`, `TAPI32.DLL`（モデム対戦）, `m2e\Br_ipso.cpp`, `m2e\Shuku2.cpp` |

共通:
- MFC アプリ（メニューバー付きウィンドウ）。リンカ 5.0（Visual C++ 5.0）
- imports: `DDRAW`, `DSOUND`, `WINMM`（`timeGetTime`, `mciSendCommandA`）, `DPLAYX`, `DINPUT`（`DirectInputCreateA`）
- DirectInput: `IID_IDirectInputDevice2A` と `GUID_ConstantForce` を保持 → **DirectInput 5 世代、FFB 実装あり**（`Microsoft SideWinder Force Feedback Pro` 等の文字列）
- ソースパス `D:\Stc\StcPc\Src\Saturn\game.c`, `...\Saturn\Car\meter.c` → **Saturn 版コードベースの移植**
- `D:\Stc\StcPc\m2e\*.cpp` → Model 2 描画系を PC 向けに再実装した層と推定（`M2E_STCF.BIN`）
- リプレイ・ゴースト: `REPLAY/REPLAY%02d.DAT`, `AIFILE/AICAR%02d.DAT`

### 意味するところ
1. **.reloc が無い＝ exe 内のアドレスは常に固定。** 静的領域のゲーム状態はポインタを辿らずに直接読める。
2. **float の 4/3・3/4、640/480 定数がどこにも無い。** Saturn 由来で固定小数点演算の可能性が高い（16.16 の 0.75 = `0x0000C000` は .rdata に 25 箇所あるがノイズも多い）。ワイド化ではアスペクト値を「実行時に計算されている固定小数点」として追う必要がある。
3. D3D は DX5 の `IDirect3D2` 系。dgVoodoo2 の DirectDraw/D3D5 対応範囲内。

## 3. 未解決

- `DirectInputCreateA` の呼び出し箇所（IAT 直接呼び出しでは見つからず、サンク経由と推定）→ Ghidra で確認
- `.MRG` / `.BIN` のフォーマット
- インストール後フォルダとの差分（Phase 2 で `-Compare disc,installed`）
