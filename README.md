# Wheelman Mod

An ImGui overlay / trainer for **Wheelman** (2009, Midway) — a DLL injected into the running game that adds a settings
menu, cheats, camera tweaks and a custom post-processing pipeline, without modifying any game file.

Not affiliated with or endorsed by Midway Games, Tigon Studios or Warner Bros. "Wheelman" and related names are
trademarks of their respective owners, used here only to describe what the mod is for. See
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for the full licence picture, including why no game files are
shipped here. You need your own legal copy of the game.

Русская версия ниже: [README на русском](#wheelman-mod-русский).

---

## What it does

- **Player** — god mode / health, noclip & fly, health regeneration, weapon list & ammo, a direct-mouse camera mode
  (turns the camera straight from raw mouse input instead of the game's emulated controller stick, with separate
  sensitivity while aiming), movement tweaks, live pawn flags.
- **Vehicle** — repair, durability, top speed unlock, driving/handling tweaks, horn & siren, special abilities,
  damage/power tuning, and a full vehicle catalog spawner (any vehicle in the game, loaded from its own package on
  demand) with cleanup for spawned cars.
- **Combat** — enemy/ally stealth and invincibility controls, an experimental auto-shot/aim assist.
- **World** — mission timer, wanted level and police controls, weapon/ammo cache and garage helpers, HUD toggles.
- **Map** — an external minimap window built from your own game files (city picture, road mask, mission/event
  markers, icon set), plus an overlay on the game's own in-HUD minimap.
- **Visuals** — a from-scratch post-processing stack (FXAA, sharpening, colour grading, ambient occlusion, distance
  haze, depth of field) built directly on the game's D3D9 depth buffer, with sane defaults; advanced sliders are
  hidden behind an "unlock" checkbox so you don't have to touch them.
- **Built-in cheats** — the game's own hidden cheat codes, exposed as menu buttons.
- **Settings** — language (English/Russian), hotkeys, save/load config profiles, a Debug switch that reveals raw
  internals for people who want to poke at the game's memory layout.

A separate small **Launcher** (`WheelmanModLauncher.exe`) starts the game and injects the mod for you, and can also
generate the map data described below.

## Installing

1. Build the project (see [Building](#building)), or get a release build if one is published.
2. You need the map data (`MapData\citymap.dxt1`, `icons.bgra`, `roads.bin`) for the map features to show anything.
   It is **not included** — it's extracted from your own game files. Either:
   - click **"Prepare the map and icons from my game"** in the Launcher, or
   - run `tools\MapDataBuilder\bin\MapDataBuilder.exe <path to Wheelman.exe or game folder> <output folder>` yourself
     and copy the result next to `WheelmanMod.dll` as `MapData\`.
3. Run `WheelmanModLauncher.exe`, point it at your `Wheelman.exe`, and start the game through it. It injects
   `WheelmanMod.dll` automatically.
4. In-game, open the overlay with its hotkey (configurable in Settings → Hotkeys, see the launcher/overlay for the
   current default).

**⚠️ The Launcher and the mod have only been tested against the Russian release of the game.** Starting the game
with `-language=int` (the English build) has not been verified — file offsets, string tables or the map data
extraction may behave differently. If you test it on the English version, please report back with what works and
what doesn't.

### Uninstalling

Just stop using the Launcher / injector — the mod only exists inside the game's process memory while it's running
and never touches your game installation or save files. Delete the mod's folder and, if you generated it, the local
`MapData\` folder.

## Building

Requirements: Visual Studio (2022 or newer) with the "Desktop development with C++" workload, Windows 10/11, a
32-bit toolchain (the game and the mod are both x86).

```
msbuild WheelmanMod.sln /p:Configuration=Release /p:Platform=Win32
```

This builds `WheelmanMod.dll`, `Injector.exe` and `WheelmanModLauncher.exe` into `bin\`. The `Injector` reads its
target exe path from an optional `Injector.ini` next to it (`[Injector] GameExe=... WorkingDir=...`); without one it
defaults to `Wheelman.exe` in the current folder.

`tools\MapDataBuilder` builds separately and needs no Visual Studio — run `tools\MapDataBuilder\build.cmd` (uses the
.NET Framework compiler that ships with Windows).

## Contributing

This is meant to be a genuinely open, hackable project — clone it, change it, send a pull request, fork it, whatever
is useful to you. A few practical notes:

- Game files, decompiled scripts, `MapData\`, and anything else covered by `.gitignore` must never be committed —
  see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for why.
- New menu options/sliders should be registered with the `Binds` config system (`RegisterToggle`/`RegisterFloat`/
  `RegisterInt`) so they get saved in the user's config profile like everything else.
- New user-facing strings should get a Russian translation added to `Lang_ru.inc`.
- Offsets and struct layouts in this codebase were found by reverse engineering the shipped exe/UPKs for
  interoperability; keep that spirit — no leaked source, no ripped assets.

## Licence

The mod's own code is MIT-licensed, see [LICENSE](LICENSE). Third-party components and the legal notes on the game
itself are in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

---

# Wheelman Mod (русский)

Оверлей/трейнер для игры **Wheelman** (2009, Midway) — DLL, которая внедряется в запущенную игру и добавляет меню
настроек, читы, управление камерой и собственный постпроцессинг картинки, не изменяя ни одного файла игры.

Проект не связан с Midway Games, Tigon Studios или Warner Bros. и не одобрен ими. Названия "Wheelman" и связанные
с игрой имена — товарные знаки их владельцев, используются здесь только чтобы объяснить, для чего сделан мод.
Подробнее о лицензиях и юридической стороне — в [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md). Для игры
нужна ваша собственная легальная копия.

## Что умеет мод

- **Player** — бессмертие/здоровье, ноклип и полёт, регенерация здоровья, список оружия и патронов, прямое
  управление камерой мышью (камера поворачивается напрямую от сырого ввода мыши, а не через эмуляцию стика
  геймпада, с отдельной чувствительностью при прицеливании), настройки движения, живые флаги персонажа.
- **Vehicle** — ремонт, прочность, снятие ограничения скорости, настройки управляемости, гудок и сирена,
  спецспособности, настройка урона/мощности, а также спавнер любого транспорта из игры (загружается из
  собственного пакета по запросу) с очисткой заспавненных машин.
- **Combat** — скрытность и неуязвимость врагов/союзников, экспериментальный автоприцел.
- **World** — таймер миссии, уровень розыска и управление полицией, помощники по тайникам оружия/патронов и
  гаражам, переключатели HUD.
- **Map** — отдельное окно миникарты, собранное из файлов вашей же игры (картинка города, маска дорог, метки
  миссий/событий, набор иконок), а также наложение поверх встроенной миникарты игры.
- **Visuals** — постпроцессинг картинки с нуля (FXAA, резкость, цветокоррекция, ambient occlusion, дальняя дымка,
  глубина резкости) на основе буфера глубины D3D9 самой игры, с разумными настройками по умолчанию; тонкие
  настройки спрятаны за чекбоксом "разблокировать", чтобы их не трогали без нужды.
- **Built-in cheats** — собственные скрытые чит-коды игры в виде кнопок меню.
- **Settings** — язык (английский/русский), горячие клавиши, сохранение/загрузка профилей настроек, переключатель
  Debug, открывающий "сырые" технические подробности для тех, кто хочет покопаться в памяти игры.

Отдельный небольшой **Launcher** (`WheelmanModLauncher.exe`) запускает игру и внедряет мод, а также умеет собрать
данные карты (см. ниже).

## Установка

1. Соберите проект (см. [Сборка](#сборка)) либо возьмите готовую сборку, если она опубликована.
2. Для карты нужны данные (`MapData\citymap.dxt1`, `icons.bgra`, `roads.bin`) — без них функции карты ничего не
   покажут. Они **не включены** в репозиторий — их надо собрать из файлов вашей игры. Либо:
   - нажмите **«Подготовить карту и иконки из моей игры»** в лаунчере, либо
   - запустите `tools\MapDataBuilder\bin\MapDataBuilder.exe <путь к Wheelman.exe или папке игры> <папка вывода>`
     вручную и скопируйте результат рядом с `WheelmanMod.dll` в папку `MapData\`.
3. Запустите `WheelmanModLauncher.exe`, укажите путь к `Wheelman.exe` и запускайте игру через него — DLL внедрится
   автоматически.
4. В игре откройте меню мода горячей клавишей (настраивается в Settings → Hotkeys, значение по умолчанию видно
   в лаунчере/оверлее).

**⚠️ Лаунчер и мод проверялись только на русской версии игры.** Запуск с параметром `-language=int` (английская
сборка) не проверялся — смещения в файлах, таблицы строк или сборка данных карты могут вести себя иначе. Если
проверите на английской версии — сообщите, пожалуйста, что сработало, а что нет.

### Удаление

Достаточно просто не запускать лаунчер/инжектор — мод существует только в памяти запущенного процесса игры и не
трогает саму установку игры или сохранения. Можно удалить папку мода и, если она была создана, локальную папку
`MapData\`.

## Сборка

Нужны: Visual Studio (2022 или новее) с компонентом "Разработка классических приложений на C++", Windows 10/11,
32-битный набор инструментов (игра и мод — x86).

```
msbuild WheelmanMod.sln /p:Configuration=Release /p:Platform=Win32
```

Соберутся `WheelmanMod.dll`, `Injector.exe` и `WheelmanModLauncher.exe` в `bin\`. `Injector` читает путь к игре из
необязательного `Injector.ini` рядом с собой (`[Injector] GameExe=... WorkingDir=...`); без него по умолчанию
берётся `Wheelman.exe` в текущей папке.

`tools\MapDataBuilder` собирается отдельно и не требует Visual Studio — запустите
`tools\MapDataBuilder\build.cmd` (использует компилятор .NET Framework, который есть в самой Windows).

## Участие в разработке

Проект задумывался как по-настоящему открытый: клонируйте, меняйте, присылайте pull request, форкайте — как вам
удобнее. Несколько практических замечаний:

- Файлы игры, декомпилированные скрипты, `MapData\` и всё остальное, что в `.gitignore`, коммитить нельзя — почему,
  написано в [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
- Новые пункты меню/ползунки стоит регистрировать в системе конфигов `Binds` (`RegisterToggle`/`RegisterFloat`/
  `RegisterInt`), чтобы они сохранялись в профиле настроек пользователя вместе со всем остальным.
- Для новых строк интерфейса желательно добавить русский перевод в `Lang_ru.inc`.
- Смещения и структуры в этом коде найдены реверс-инжинирингом exe/UPK игры ради совместимости — придерживайтесь
  того же духа: никаких слитых исходников, никаких вытащенных ассетов в репозитории.

## Лицензия

Код самого мода — под лицензией MIT, см. [LICENSE](LICENSE). Сторонние компоненты и юридические заметки про саму
игру — в [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
