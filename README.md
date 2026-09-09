# th08-psp-native — 東方永夜抄 ～ Imperishable Night, native PSP port

**東方永夜抄（TH08）の非公式PSPネイティブ移植です。** PSP-2000 / PSP-3000 / PSP Go（メインメモリ64MB機）で、
原作のゲームロジックをそのままPSP上で走らせます。エミュレーションではなく、PC版のデコンパイル（[GensokyoClub/th08](https://github.com/GensokyoClub/th08) と、そこから枝分かれした [N0zoM1z0/th08](https://github.com/N0zoM1z0/th08)）を
PSPのGE（グラフィックス）／SC（メインCPU）向けに移植したものです。

**Unofficial native PSP port of Touhou 8 (Imperishable Night).** Runs the original game logic natively on
PSP-2000 / PSP-3000 / PSP Go (64 MB models). Not an emulator: it is a port of the PC decompilation to the PSP's GE and main CPU.

> **Note:** This port is based on an AI-generated decompilation of TH08 that is not affiliated with or endorsed by Gensokyo Club. Gensokyo Club does not provide any support for this project. For the community-maintained decompilation, please refer to the official Gensokyo Club repositories.

> 2026-09-09: [v0.3.0-beta / r238 ログなし版](https://github.com/kan8223-dotcom/th08-psp-native/releases/tag/v0.3.0-beta)
> - 実機へ投入した r238 をそのまま公開。プレイヤーから全体的なパフォーマンス向上の報告があります。新しい性能ログは記録しないため、FPS改善率の数値比較はありません。
> - MEによる弾・効果・背景などの処理、GE送信の集約、HUD頂点の記録再生、文字ラスタライズの再利用を含みます。音声処理はSC側です。
> - シミュレーションは60 Hzを目標に維持し、描画は負荷に応じて60/30/20へ自動調整。SELECTで手動選択できます。ボム中は自動設定時に20描画へ切り替わります。
> - **会話早送りはまだ重い**など、最適化は継続中です。対象のボス立ち絵は空き容量がある場合のみ下位eDRAMを優先し、足りなければMain RAMへ戻します。
> - 今回の実機報告はPSP Go・M2環境。前段のログなしr236は423 MHzでリプレイ完走報告があり、443 MHzでは停止報告があります。これをr238や他機種の完走保証にはしていません。オーバークロックの安定動作は保証しません。
> - 内蔵ストレージの読み込み停止問題は未解決です（[詳細](docs/psp-go-internal-storage-stall.md)）。32bitの顔・背景テクスチャは16bit化／ラストワード練習・PSP-1000は非対応。
>
> r238 is the exact no-log build installed on the test PSP Go. The player reports substantially better overall performance, but dialogue fast-forward remains slow. No new performance logs are recorded and no measured FPS gain is claimed. Earlier-build playthrough results are not a guarantee for this build or other models; overclocking is not guaranteed stable. See the release notes for validation limits.

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
更新時は設定・スコア・`replay/`を残し、配布ファイルだけを置き換えてください。
**r238はログなし版です。新しい `TH08PSP_BOOT.LOG` や性能ログは書きません。**
以前のログが残っていても今回の実行記録ではありません。不具合報告にはビルド名・機種・ストレージ・クロック・発生場面を添えてください。
セーブ・リプレイ保存は無効化していません。ME用の `kcall.prx` は起動時に本体から展開されるので、別途ダウンロードは不要です。

## ビルド / Build

PSPSDK（psp-gcc 15.2で検証）と SDL2 / SDL2_image / SDL2_ttf のPSPビルド、CMake、Python 3、xxdが必要です。
`PSPDEV`を設定し、PSPSDKのコマンドをPATHに入れたクリーンなチェックアウトで実行します。

```
bash tools/build_r238_nolog_20260909_183252.sh
```

このスクリプトにr238のfeature vectorを固定しています。ローカルな`build/`内の履歴スクリプトは不要です。
最適化・観測機能は `Makefile.psp` の `TH08_PSP_*` スイッチで個別に選択できます。
PSPGLは `deps/pspgl-ge4/` の凍結stream-listアーカイブ（BSD-3）、MEライブラリは `psp/third_party/me-custom-core/`（MIT）を使います。
公開バイナリの識別値とビルド検証範囲は [r238公開記録](docs/releases/r238_20260909_183252.md) を参照してください。

## 技術メモ / Technical notes

- ゲーム全体の進行・音声はSC側、対象の弾更新や描画用データ生成などをME workerに分担させています。
- GE 側 4 MiB eDRAM（PSP-2000 以降）を `ge4wrap_texv1.prx` で解錠し、上位 2 MiB をテクスチャ昇格に使います。
- GE stream-list、トリプルバッファ、HUD前面スプライトの記録再生と文字送信の集約を使います。
- r237の文字マスク再利用は縁取り4回＋本体1回の同じ文字ラスタライズを再利用します。行をまたぐ会話全文キャッシュではありません。
- ログ出力は無効ですが、RAM上の一部計測カウンタは残ります。「すべてのI/O・計測コストがゼロ」という構成ではありません。
- 計測・判定の記録は `TH08_PSP_ISSUE_LEDGER.md` / `TH08_PSP_PORT_PLAN.md`（作業リポジトリ側）にあります。

## クレジット / Credits

- Original game: 東方永夜抄 © 上海アリス幻樂団 (Team Shanghai Alice). This is an unofficial fan port. No original assets are distributed.
- PC decompilation (two upstream projects): the original reconstruction [GensokyoClub/th08](https://github.com/GensokyoClub/th08) (KSS, MIT), and its fork [N0zoM1z0/th08](https://github.com/N0zoM1z0/th08) (Linux/portable64 port, MIT), which is the direct base of this PSP port.
- PSPGL fork base: [pspdev/pspgl](https://github.com/pspdev/pspgl) (BSD-3-Clause), SDL2 for PSP, PSPSDK, ARK CFW.
- Media Engine custom core: m-c/d (MIT); source, upstream notes and license are preserved in `psp/third_party/me-custom-core/`.
- PSP eDRAM (4 MiB) knowledge and hardware discussion: **m-c/d** and **Acid_Snake** of the PSP Homebrew Community. Thank you.
- Port engineering: kan82 with coding agents (OpenAI Codex, Anthropic Claude). See `th07-psp-native` for the sibling Touhou 7 port.

## License

MIT (see `LICENSE`). Third-party notices are in `licenses/`.
