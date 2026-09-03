# th08-psp-native — 東方永夜抄 ～ Imperishable Night, native PSP port

**東方永夜抄（TH08）の非公式PSPネイティブ移植です。** PSP-2000 / PSP-3000 / PSP Go（メインメモリ64MB機）で、
原作のゲームロジックをそのままPSP上で走らせます。エミュレーションではなく、PC版のデコンパイル（[GensokyoClub/th08](https://github.com/GensokyoClub/th08) と、そこから枝分かれした [N0zoM1z0/th08](https://github.com/N0zoM1z0/th08)）を
PSPのGE（グラフィックス）／SC（メインCPU）向けに移植したものです。

**Unofficial native PSP port of Touhou 8 (Imperishable Night).** Runs the original game logic natively on
PSP-2000 / PSP-3000 / PSP Go (64 MB models). Not an emulator: it is a port of the PC decompilation to the PSP's GE and main CPU.

> 2026-09-04 時点の状態 / Status as of 2026-09-04
> - 実機（PSP-3000, 333 MHz）で **1面がプレイ可能**。内蔵デモプレイ4本は最後まで完走します。
> - 60 Hz simulation × 30描画（SELECTで60/30/20描画を切替）。1面道中で約47〜58 sim fps、5面デモで約54 sim fps。
> - **既知の問題**: ボス撃破後、2面のロードで停止する／会話中に背景が黒くなる（実機のみ）／デモの吸血鬼組（咲夜・レミリア）のリプレイが非同期。
> - PSP-1000（32 MB）は非対応。
> - Stage 1 is playable on real hardware (PSP-3000 @ 333 MHz); all four built-in demo plays run to completion.
> - Known issues: the game stops when loading Stage 2 after the Stage 1 boss; the background goes black during dialogue (hardware only); the Sakuya/Remilia demo replay desyncs. PSP-1000 (32 MB) is not supported.

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
