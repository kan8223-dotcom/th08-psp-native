# PSP Go 内蔵ストレージ（ef0）の30秒停止について / PSP Go internal storage (ef0) 30-second stalls

## 現象 / Symptom

PSP Go の内蔵ストレージ（`ef0:`）で本作を動かすと、会話開始・ステージ開始・ボス登場（BGM切替）のタイミングで
本体がきっかり **30.0 秒** 止まることがありました。止まっている間も PS ボタンは効き、待てば復帰します。
読みが失敗した場合は「敵データの読み込みに失敗しました」でタイトル／XMB へ戻ります。
PSP-3000 のメモリースティック（`ms0:`）では発生しません。CPU クロック（333/423/443 MHz）にも無関係でした。

On the PSP Go's internal storage (`ef0:`) the game could freeze for exactly **30.0 seconds** when a
conversation starts, a stage begins, or a boss appears (BGM switch). The PS button still works and the
game resumes afterwards; if the read fails instead, the game exits with an enemy-data load error.
It never happens on a PSP-3000 with a Memory Stick (`ms0:`), and the CPU clock is irrelevant.

## 原因 / Cause

内蔵ストレージのドライバは、**「seek → 数バイトの read」を短時間に連打されると、その直後の別の read を
30 秒待たせる**癖があります（専用プローブで再現: 極小 read 自体は 1 ms 以下、直後の 44,100 バイト read が
30,008 ms、150 回中 24 回）。本作ではフォント描画（SDL_ttf / FreeType）がグリフを読むたびに
`lseek(0, SEEK_CUR)` → `lseek(pos)` → `read(2〜300 B)` を十数回連打しており、それが引き金でした。
巻き添えになるのは BGM（`thbgm.dat`）、ステージデータ（`th08.dat`）、フォント自身のどれかで、
どのスレッドの読みでも起きます。

The ef0 driver has a quirk: **after a burst of tiny seek+read operations, the next read (on any file, from
any thread) is held for 30 seconds.** A standalone probe reproduces it: the tiny reads themselves take
under 1 ms, but the following 44,100-byte read takes 30,008 ms (24 of 150 bursts). In this port the
trigger was glyph loading by SDL_ttf/FreeType, which issues `lseek(0, SEEK_CUR)` → `lseek(pos)` →
`read(2..300 bytes)` a dozen times in a row whenever new text is drawn.

## 対策 / Fix

r141 以降、フォントファイルの読みは 4 KiB × 8 ブロックのキャッシュ経由にし、ドライバへは 4 KiB 整列の
read しか出しません（ビルドスイッチ `TH08_PSP_FONT_STREAM_CACHE`、`TH08_PSP_IO_SERIALIZE` と併用）。
これで音楽室のカーソル移動、会話開始、1〜3 面のステージロードで停止は出なくなりました。
PSP Go 専用のメモリースティック（M2）を用意する必要はありません。

Since r141 the font file is served through an 8 × 4 KiB block cache inside the port
(`TH08_PSP_FONT_STREAM_CACHE`, used together with `TH08_PSP_IO_SERIALIZE`), so the driver only ever sees
4 KiB-aligned reads. Music Room cursor moves, dialogue starts and stage loads no longer stall. A
dedicated Memory Stick Micro (M2) for the Go is not required.

## 切り分けの記録 / What was ruled out

- CPU/バスクロック（333・423・443 MHz） / CPU and bus clock
- GE 4 MiB eDRAM ラッパー PRX（無効化しても再発） / the GE 4 MiB eDRAM wrapper (still stalled with it disabled)
- 複数スレッドの同時 I/O（直列化しても再発） / concurrent I/O from several threads (still stalled when serialized)
- 読み先バッファの位置（拡張メモリ／通常メモリ）と整列 / destination buffer location and alignment
- 音声出力・GE・CPU 負荷、`scePowerSetClockFrequency` 呼び出し / audio, GE and CPU load, the clock call
- 連続読み・512 B ランダム読み・BGM 型読み・アーカイブ型 open/seek/read/close・再 open・書込直後の読み / sequential,
  512-byte random, BGM-like, archive-like, reopen and write-then-read patterns (none reproduce it)

再現ツール: `tools/io_probe/`（`TH08IOPROBE`、フェーズ `P17`/`P18` が極小 read の連打）。
Repro tool: `tools/io_probe/` (`TH08IOPROBE`; phases `P17`/`P18` are the tiny-read bursts).
