# th08-psp-native — 東方永夜抄 ネイティブPSP移植

東方永夜抄（TH08）の非公式ファン移植です。原作データは配布しません。
PC版デコンパイルをPSPのSC（メインCPU）・GE（描画）向けに移植したもので、PCエミュレーターではありません。

> This port is based on an AI-generated decompilation of TH08 that is not affiliated with or endorsed by Gensokyo Club. Gensokyo Club does not provide support for this project. For the community-maintained decompilation, refer to the official Gensokyo Club repositories.

## 最新版: v0.4.0 / Go r249 MECC音声版

[リリースとダウンロード](https://github.com/kan8223-dotcom/th08-psp-native/releases/tag/v0.4.0)

- **MECC（Media Engine Custom Core、m-c/d、MIT）を使用して音声ミキシングをMEへ移しました。** 非音声MEは無効で、ゲーム進行・描画用データ生成はSC側です。
- PCM・出力・スタックはMain RAM。**MEローカルeDRAM/MIST不使用。** 描画用のGE側4MiB eDRAMとは別です。
- ネイティブGE描画、三角関数の節点表、スプライト転送削減、会話テキストのキャッシュ、音楽室コメント更新の軽量化を含みます。
- **公開版は診断ログとCPU使用率グラフを無効化。** ゲーム本来のFPS表示、セーブ、リプレイ保存は残します。
- MEの起動時自己検査、失敗時のSCフォールバック、タイムアウト保護、安全な終了処理は残しています。

今回の対象・実機確認環境は **PSP Go / M2 / ARK / 423MHz** です。
この新しいネイティブ描画版のPSP-2000/3000動作は未確認、PSP-1000は非対応です。
オーバークロックの安定性を保証しません。内蔵ストレージの停止問題は
[従来の注意事項](docs/psp-go-internal-storage-stall.md)を参照してください。
ラストワード練習非対応、16bitテクスチャ化などの制約も継続です。

ログありr249はGo実機で6Bを完走し、26,804回すべてME音声処理、フォールバック・タイムアウト0を確認しました。
検証範囲は [r249公開記録](docs/releases/r249_20260922.md) を参照してください。
ログあり実機版と、ログ・グラフなし公開バイナリは別ビルドです。
PPSSPPはMEをスキップするため、エミュレーターの動作確認だけで実機MEの速度を証明してはいません。

## 導入

配布ZIPの `TH08PSP` フォルダを `ms0:/PSP/GAME/` へ置きます。
ご自身の正規版から `th08.dat` と `thbgm.dat` を追加してください。

```text
ms0:/PSP/GAME/TH08PSP/
  EBOOT.PBP
  ge4wrap_texv1.prx
  NotoSansJP-Regular.ttf
  th08.dat                 自分の正規版から
  thbgm.dat                自分の正規版から
```

更新時は設定・スコア・`replay/`を残し、配布物だけを置き換えてください。
MECC用の `th08_audio_kcall.prx` は起動時にEBOOTから展開され、別途導入不要です。
古い `kcall.prx` は上書きしません。以前のログも消去・更新しないため、残っていても公開版の実行記録ではありません。
個人所有の `msgothic-subset.ttf` があれば優先しますが、MSフォントや派生フォントは配布しません。

SELECTで描画レート60/30/20を切り替えられます。自動時は負荷に応じて調整し、ボム中は20描画です。
シミュレーションは60Hzを目標としますが、常時60描画を保証しません。
不具合報告にはビルド名・機種・ストレージ・クロック・発生場面を添えてください。

## ビルド

PSPDEV/PSPSDK、psp-gcc、PSP SDL2/image/ttf、CMake、Python 3、xxdが必要です。
リリースのソースチェックアウトで実行します。原作データはビルドには不要です。

```sh
JOBS=8 bash tools/build_go_r249_release_20260922.sh
```

既定でcleanビルドを行います。設定と継承レシピはすべて公開ソースに含まれます。
今回の製品はPSPGL/EGL/OpenGLをリンクしません。古い版の再現用ファイルは履歴として保持します。
診断出力は停止していますが、一部RAM計測カウンタは残ります。全計測コストがゼロという意味ではありません。

## クレジットとライセンス

- 原作: 東方永夜抄 © 上海アリス幻樂団（Team Shanghai Alice）。非公式移植です。
- PC再構築の原点: [GensokyoClub/th08](https://github.com/GensokyoClub/th08)（KSS、MIT）。
- 直接の移植元: [N0zoM1z0/th08](https://github.com/N0zoM1z0/th08)（Linux/portable64対応、MIT）。両者の履歴と帰属を保持しています。
- **MECC / PSP Media Engine Custom Core: m-c/d、Copyright (c) 2025 m-c/d、MIT。** ソース・上流説明・MIT全文は `psp/third_party/me-custom-core/`。本移植では音声専用、MainRAMスタック、独立PRX名、callback復元、安全な停止を追加しています。
- PSPSDK、PSP SDL2/image/ttf、ARK CFW。従来PSPGL版の基盤は [pspdev/pspgl](https://github.com/pspdev/pspgl)（BSD-3-Clause）。
- GE側4MiB eDRAMの知見・検証への貢献: m-c/d、Acid_Snake、PSP Homebrew Community。
- 移植: kan82、OpenAI Codex、Anthropic Claude。

MIT（[LICENSE](LICENSE)）。第三者の全文ライセンスは `licenses/` に同梱しています。
This software uses the FreeType Project font engine. This software is based in part on the work of the Independent JPEG Group.
Noto Sans JPはSIL OFL。静的リンクするpthread-embeddedはLGPLで、対応ソースをリリースに添付しています。
そのソースで `make -C platform/psp`、`make -C platform/psp install` を実行後、アプリを再ビルドして改変ライブラリへ再リンクできます。
そのための改変・デバッグをこの移植は制限しません。
