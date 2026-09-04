# th08-psp-native — 東方永夜抄 ～ Imperishable Night, native PSP port

**東方永夜抄（TH08）の非公式PSPネイティブ移植です。** PSP-2000 / PSP-3000 / PSP Go（メインメモリ64MB機）で、
原作のゲームロジックをそのままPSP上で走らせます。エミュレーションではなく、PC版のデコンパイル（[GensokyoClub/th08](https://github.com/GensokyoClub/th08) と、そこから枝分かれした [N0zoM1z0/th08](https://github.com/N0zoM1z0/th08)）を
PSPのGE（グラフィックス）／SC（メインCPU）向けに移植したものです。

**Unofficial native PSP port of Touhou 8 (Imperishable Night).** Runs the original game logic natively on
PSP-2000 / PSP-3000 / PSP Go (64 MB models). Not an emulator: it is a port of the PC decompilation to the PSP's GE and main CPU.

> **Note:** This port is based on an AI-generated decompilation of TH08 that is not affiliated with or endorsed by Gensokyo Club. Gensokyo Club does not provide any support for this project. For the community-maintained decompilation, please refer to the official Gensokyo Club repositories.

> 2026-09-05 時点の状態 / Status as of 2026-09-05
> - PSP Go（423 MHz）実機で **1面から6面まで通しプレイし、Normal エンディングまで到達**（処理落ち率 15.4%）。PSP-3000 には同じビルドを投入済み、実機確認はこれからです。
> - 60 Hz simulation × 30描画（SELECTで60/30/20描画を切替）。密集スペルでは弾更新のCPU負荷で処理落ちします。
> - **既知の問題**: PSP Go 内蔵ストレージ（ef0）では 30 秒の読み込み停止が1周に数回残ります（[docs/psp-go-internal-storage-stall.md](docs/psp-go-internal-storage-stall.md)。メモリースティックからの起動で回避できる見込み）／32bitの顔・背景テクスチャは16bitに減色／ラストワード練習は非対応／PSP-1000（32 MB）は非対応。
> - Full playthrough to the Normal ending on a PSP Go at 423 MHz (15.4% slowdown counter). The same build is installed on a PSP-3000; hardware check pending.
> - Known issues: a few 30-second read stalls per playthrough on the PSP Go's internal storage (ef0; see the doc above, expected to be avoided by running from a Memory Stick); 32-bit face/background textures are stored as 16-bit; Last Word practice is unsupported; PSP-1000 (32 MB) is not supported.

## 必要なもの / Requirements

- CFW導入済みのPSP-2000 / 3000 / Go（ARK-4 / ARK-5 で検証。**VSHのクロック強制設定（`vsh, cpuclock`）は外してください**）
- **原作『東方永夜抄』の `th08.dat` と `thbgm.dat`**。このリポジトリにもリリースにも原作データは一切含まれません。お手持ちの正規版から取り出してください。
- フォント: リリース同梱の `NotoSansJP-Regular.ttf`（SIL OFL）。`msgothic-subset.ttf` が同じフォルダにあればそちらを優先しますが、MSゴシックは配布できません。

## インストール / Install

`ms0:/PSP/GAME/TH08PSP/` に以下を置きます:

```
EBOOT.PBP               (release asset)
ge4wrap_texv1.prx       (release asset; GE 4 MiB eDRAM bridge)
NotoSansJP-Regular.ttf  (release asset, OFL)
th08.dat                (your own copy)
thbgm.dat               (your own copy)
```

起動後はタイトルで放置するとデモが走ります。SELECTで描画レート（60/30/20）を切り替えられます。
`TH08PSP_BOOT.LOG` が同じフォルダに書かれます（不具合報告の際に添付してください）。

## ビルド / Build

PSPSDK（psp-gcc 15.2 で検証）と SDL2 / SDL2_image / SDL2_ttf の PSP ビルドが必要です。

```
make -f Makefile.psp clean
make -f Makefile.psp -j4 TH08_PSP_BUILD_ID=<id> <feature vector>
```

リリース版の feature vector は各リリースノートに記載しています（`TH08_REPLAY_SYNC_AUDIT=0` を必ず指定）。
すべての最適化・観測機能は `Makefile.psp` の `TH08_PSP_*` スイッチ（既定OFF）で個別に有効化され、
`tools/test_psp_*.py` のソース契約テストで検証されます。PSPGLは `deps/pspgl-ge4/` に凍結したフォーク（BSD-3）を使います。

## 技術メモ / Technical notes

- SC-only 構成です。Media Engine（ME）は使用していません（`FEATURE ME=DISABLED` をブートログで確認できます）。
- GE 側 4 MiB eDRAM（PSP-2000 以降）を `ge4wrap_texv1.prx` で解錠し、上位 2 MiB をテクスチャ昇格に使います。
- 弾・アイテムの頂点生成は GE へ直接（no-copy）投入、Item/三角関数は bit-exact な double-float 高速経路、
  描画は SWAP_NOWAIT（VBlank 待ち除去＋表示同一性ガード）など。決定論（リプレイ同期）を壊す最適化は採用していません。
- 計測・判定の記録は `TH08_PSP_ISSUE_LEDGER.md` / `TH08_PSP_PORT_PLAN.md`（作業リポジトリ側）にあります。

## クレジット / Credits

- Original game: 東方永夜抄 © 上海アリス幻樂団 (Team Shanghai Alice). This is an unofficial fan port. No original assets are distributed.
- PC decompilation (two upstream projects): the original reconstruction [GensokyoClub/th08](https://github.com/GensokyoClub/th08) (KSS, MIT), and its fork [N0zoM1z0/th08](https://github.com/N0zoM1z0/th08) (Linux/portable64 port, MIT), which is the direct base of this PSP port.
- PSPGL fork base: [pspdev/pspgl](https://github.com/pspdev/pspgl) (BSD-3-Clause), SDL2 for PSP, PSPSDK, ARK CFW.
- PSP eDRAM (4 MiB) knowledge and hardware discussion: **m-c/d** and **Acid_Snake** of the PSP Homebrew Community. Thank you.
- Port engineering: kan82 with coding agents (OpenAI Codex, Anthropic Claude). See `th07-psp-native` for the sibling Touhou 7 port.

## License

MIT (see `LICENSE`). Third-party notices are in `licenses/`.
