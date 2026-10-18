# ps5-syslang-lite

`ps5-syslang-lite` switches the PS5 system language by writing **one registry
value** and nothing else:

```c
sceRegMgrSetInt(0x02020000, language_id);
```

It exists because of a J1 (JP region-locked) console, where the original
`ps5-syslang` approach — let the managed `ChangeSystemLanguage` /
`ReloadApplicationForChangeSystemLanguage` run inside SceShellUI — freezes the
console and makes it rebuild its whole application database.

Everything below was measured on that console: the freeze, the database
rebuild and the stale home screen are observations, not speculation.

## What this build does

* writes the target language id into `0x02020000`,
* reads it back and reports the result as a notification,
* reports again 6 s later, once the system service has re-read the registry,
* **if the registry already holds the target language, it says so and writes
  nothing** (nothing is re-applied, nothing is touched).

## What this build deliberately does not do

* no injection into SceShellUI (no `elfldr`, no `pt_attach`),
* no managed reload (`ReloadApplicationForChangeSystemLanguage`),
* no scene reload (`ReactApplicationSceneManager.ReloadApp`),
* no ShellUI restart, no navigation.

All three of the rejected approaches above caused problems on the J1 console
(freeze + database rebuild; harmless no-op; UI left broken), which is exactly
why they are gone from this build instead of hidden behind a flag.

## The one known limitation

The **already rendered** home screen keeps the old language. Text that is
rendered *after* the switch is in the new language (apps, the notification
area, a settings page that is entered again) — only the home screen that was
already on screen is stale.

Refreshing it, without touching anything in this payload:

* re-enter the settings menu (it re-reads the language), or
* load **OnionHEN** once: at the end of its injections it reloads the home
  screen's React Native scene (`NPXS40002`) from the UI thread. This is the one
  mechanism that was observed to repaint the home screen cleanly.

## Build

Needs [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk). No
third-party sources are required.

```bash
make                      # all 31 languages + probe
make LANGUAGES="zh-Hans:11 ja:0"
make ELF_PREFIX=syslang-lite_V0.21.2
```

`make test-host` builds and runs `tests/lite_logic_test.c` with the *host*
compiler (no SDK needed). It drives the whole decision logic against a fake
registry: the "already set" early exit, the switch, the retry, and the
failed-write path.

Output:

```text
build/ps5-syslang-lite_V0.21.1-ja.elf
build/ps5-syslang-lite_V0.21.1-zh-Hans.elf
...
build/ps5-syslang-lite_V0.21.1-probe.elf
```

## Usage

Load the ELF for the target language (U disk / plugin loader / 9021 elfldr —
this payload does not care how it is started, and needs no `elfldr` running).

```text
ps5-syslang-lite_V0.21.1-zh-Hans.elf   Chinese (Simplified)
ps5-syslang-lite_V0.21.1-en-US.elf     English (United States)
```

Notifications:

| situation | notification |
|---|---|
| already in that language | `已经是简体中文` + `未做任何修改｜...` |
| switched | `已切换为简体中文 (11)` + `重进设置页或加载一次 OnionHEN 可刷新主界面` + `写入完成 reg=11 sys=简体中文(11)` |
| write failed | `写入注册表失败 set=0x...` (nothing was switched) |

Every run also logs the before/after state to the klog with the
`[ps5-syslang-lite]` prefix.

## probe

`make probe` builds a read-only diagnostic that reports the language the system
is using (`sceSystemServiceParamGetInt`) next to the registry value behind it
(`sceRegMgrGetInt` / `sceRegMgrGetIntInitVal` on `0x02020000`) and scans the
neighbouring ids. It writes nothing and is safe to run repeatedly.

| report | meaning |
|---|---|
| `reg` = written id, `sys` = something else | the registry keeps the write; the applied language is re-derived elsewhere |
| `reg` = `init` | the system clamped the registry during boot, so it has to be written again after every boot |

## Languages

Language ids are fixed at build time; no config file is read.

```text
 0 ja       Japanese                         16 pl       Polish
 1 en-US    English (United States)          17 pt-BR    Portuguese (Brazil)
 2 fr-FR    French (France)                  18 en-GB    English (United Kingdom)
 3 es-ES    Spanish (Spain)                  19 tr       Turkish
 4 de       German                           20 es-419   Spanish (Latin America)
 5 it       Italian                          21 ar       Arabic
 6 nl       Dutch                            22 fr-CA    French (Canada)
 7 pt-PT    Portuguese (Portugal)            23 cs       Czech
 8 ru       Russian                          24 hu       Hungarian
 9 ko       Korean                           25 el       Greek
10 zh-Hant  Traditional Chinese              26 ro       Romanian
11 zh-Hans  Simplified Chinese               27 th       Thai
12 fi       Finnish                          28 vi       Vietnamese
13 sv       Swedish                          29 in       Indonesian
14 da       Danish                           30 uk       Ukrainian
15 no       Norwegian
```

## Notes / warnings

* On a JP region-locked console the system language reverts to Japanese after a
  reboot (the supported-language list is effectively burnt in), so the payload
  has to be run again after every cold boot. Rest Mode keeps it.
* This payload is safe on any console, but it is only *useful* on a
  region-locked one: everywhere else the upstream flow already works.
* Nothing here needs `elfldr` on port 9021 — the payload writes one registry key.

## License

GPL-3.0-or-later. Based on [ps5-syslang](https://github.com/owendswang/ps5-syslang);
this fork drops the injection/reload machinery that was measured to break a
region-locked console.
