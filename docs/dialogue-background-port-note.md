# 永夜抄（TH08 decomp）移植で「会話中に背景が消える／黒くなる」件の原因と直し方

対象: th08 decompilation ベースの移植全般（PSP native で解決済み。Android 2.3 等でも同じ現象が出る）。

## 原因（デコンパイル由来ではなく、原作の描画設計と表示方式の不一致）

原作は `D3DSWAPEFFECT_COPY` で Present する（`src/main.cpp` の `presentParams.SwapEffect`）。COPY ではバックバッファの内容が
次フレームまで残る。原作はそれに依存して、会話中はプレイ領域（32,16〜416,464）に**何も描かない**。

`src/Background.cpp`（decomp 原本の行番号）:

```
901:  Background::OnDrawHighPrio
      if (background->spellBackgroundState <= SPELL_BACKGROUND_FADING_IN &&
          !g_Gui.IsDialoguePresent())          ← 2Dステージ絵 stageVm0/1 と stageEffect の描画をスキップ
960:  Background::OnDrawHighPrio
      同条件で RenderObjects(0) / RenderObjects(1) をスキップ
979:  Background::OnDrawLowPrio
      同条件で RenderObjects(2) / RenderObjects(3) をスキップ
```

同じ関数の直後にある `Clear` は通常 `D3DCLEAR_ZBUFFER` だけ（色は消さない）。プレイ領域の色を消すのは
`clearPending`（面開始時に1回）と `clearColor` のαが 0xFF のとき（フェード）だけ。

つまり原作PCでは「会話中は前フレームの背景がそのまま静止画として残る」。
移植先でスワップ後の内容が保証されない（EGL の既定 `EGL_BUFFER_DESTROYED`、毎フレーム色クリア、
ダブルバッファの交互表示 等）と、会話中のプレイ領域が黒／ゴミになる。

同じ理由で HUD（front.anm のタイル）も原作は「変化した時だけ描く」。移植で HUD が欠ける・ちらつく場合は
`Supervisor::IsHUDRedrawEnabled()` を常に TRUE（毎フレーム再描画）にする。原作の設定 `redrawHUDEveryFrame`
（th08.cfg の opts）と同じ意味。

## 直し方

### 案A（最短・PSP で採用）: 会話中も背景を描く

上記3箇所の条件から `!g_Gui.IsDialoguePresent()` を外す（`spellBackgroundState` の条件は残す）。

```cpp
if (background->spellBackgroundState <= SPELL_BACKGROUND_FADING_IN)
```

- 会話中も背景が動く（原作は静止）。挙動の差分だが、見た目は自然で田中さんはこちらを採用。
- `Background::OnUpdate` は会話中も走っているので、描くだけで動く。ゲーム進行（敵・弾・タイマー）は別ゲートなので影響なし。
- PSP 実装では `TH08_PSP_DIALOGUE_LIVE_BACKGROUND` スイッチで
  `(TH08_PSP_DIALOGUE_LIVE_BACKGROUND_ENABLED || !g_Gui.IsDialoguePresent())` と書いてPC側の挙動を保っている。

### 案B（原作忠実）: 前フレームの残存を再現する

B1. 論理画面（640x480）を FBO（テクスチャ）に描いているなら、**FBO の色を毎フレーム消さない**（深度だけ消す）。
    原作が色を消すのは上記 `Clear(D3DCLEAR_TARGET...)` の箇所だけなので、そこだけ FBO の色クリアに写像する。
    これが最も安く、原作と同じ絵になる。

B2. `eglSurfaceAttrib(dpy, surf, EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED)`。
    EGL 1.4 かつ config の `EGL_SURFACE_TYPE` に `EGL_SWAP_BEHAVIOR_PRESERVED_BIT` が要る。Android 2.3 世代の
    ドライバは非対応か、毎スワップで全画面コピーが入って遅い。あまり勧めない。

B3. スナップショット方式（PSP で最初に実装した方法）:
    - `g_Gui.IsDialoguePresent()` が 0→1 になったフレームで、プレイ領域の色をテクスチャへコピー
      （`glCopyTexSubImage2D`、または FBO テクスチャをそのまま保持）。
    - 会話中は **原作が描画をスキップする同じ場所**（901行の `if` の `else`、Z クリアの前）で、
      そのテクスチャをプレイ領域いっぱいの矩形として描く（Z書き込み off、α無し）。
    - フレーム先頭（BeginScene 相当）で描くと、後段の HUD 帯クリアやフォグ設定に潰されて画面に出ない
      （PSP で実際に嵌った）。必ず `Background::OnDrawHighPrio` の中で描く。
    - `clearColor` のαが 0xFF のフレーム（フェード）は原作どおり全クリアに任せる。

## 参考（PSP 側の実装箇所）

- `src/Background.cpp` 3箇所の条件 ＋ `else if` で `th08_linux_dialogue_snapshot_restore()` を呼ぶフック
- `src/modern/linux/d3d8_compat.cpp` の `CaptureDialogueSnapshot()` / `RestoreDialogueSnapshot()`
- スイッチ: `psp/dialogue_live_background.hpp`（案A）、`psp/dialogue_snapshot_at_background.hpp`（案B3）
