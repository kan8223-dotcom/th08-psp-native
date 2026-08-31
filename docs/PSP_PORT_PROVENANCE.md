# PSP port provenance and current status

## Fixed source inputs

| role | repository state |
| --- | --- |
| PSP working base | N0zoM1z0/th08 `a940881f97024c99ff387d2cb4c6650fb2bad97f` |
| TH08 authored/exact oracle | N0zoM1z0/th08 `50077aca0ed17d3bb7fee9f69a336636e2474639` |
| GensokyoClub audit reference | GensokyoClub/th08 `319729eb3765f3587f3003bbb10415b50fe216fc` |
| PSP backend reference | TH07 PSP `c11c4368bc969f98b731f46fb79067f3528511e7` |

The initial PSP platform, launch-path, data-discovery, callback, clock, and
power-keepalive shapes are selectively adapted from the CC0 TH07 PSP source.
The TH08 archive names, sizes, signatures, status model, buffered log, GU
bootstrap screen, and build gates are new for this port.

The bootstrap links no audio backend or playable TH08 engine translation unit.
It contains no original executable, archive, image, music, sound effect, replay,
score, or configuration data.

## Product status

This first artifact is only an SC-only PSP bootstrap and retail-data recognizer.
It initializes the PSP platform and GU, records memory and build information,
checks the two retail archives, renders a diagnostic status screen, and exits
through HOME, Circle, or Start.

It does not yet display the TH08 title or run game logic. A successful build is
not evidence that the EBOOT starts on a physical PSP.

**UNTESTED ON HARDWARE**

## Data contract

The bootstrap searches without modifying the original data root and expects:

| file | exact bytes | first four bytes |
| --- | ---: | --- |
| `th08.dat` | 46,838,025 | `PBGZ` |
| `thbgm.dat` | 449,961,024 | `ZWAV` |

Writable state and `TH08PSP_BOOT.LOG` stay beside the EBOOT. No retail archive
was available in the build environment, so this contract remains untested with
retail data.
