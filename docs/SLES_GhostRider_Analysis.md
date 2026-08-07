# SLES Analysis: Ghost Rider vs. Silent Hill (SHO / SHSM)

Ми проаналізували головні виконувані файли (ELF / `SLES`) для трьох ігор:

1. **Silent Hill Origins (SHO)**: `SLES_551.47`
2. **Silent Hill Shattered Memories (SHSM)**: `SLES_555.69`
3. **Ghost Rider (GR)**: `SLES_543.17`

## Results of Symbol Extraction

- **SHO**: `stripped` (символи та дебаг-інформація видалені компілятором під час релізу).
- **SHSM**: `stripped` (аналогічно).
- **Ghost Rider**: `not stripped`! Файл містить повну таблицю символів (майже **14 000 унікальних C++ символів**, включаючи простори імен, класи та методи).

Ви мали абсолютну рацію! Ghost Rider — це справжній **"Розеттський камінь" (Rosetta Stone)** для рушія Climax. Оскільки всі три ігри використовують одну й ту саму архітектуру, ми можемо використовувати символи з Ghost Rider, щоб відновити логіку, яка прихована у SHO та SHSM.

## Key Findings from Ghost Rider Symbols

Я розпакував та розпарсив мангльовані імена C++ (mangled names) і знайшов класи, які відповідають за ті самі речі, які ми намагаємось відтворити у вашому тулкіті.

### 1. RenderWare FileSystem & ARC Parsing

Як ви і припускали, обробка `.ARC` файлів лежить у головному процесорі (EE). Ось структура класів, яку нам розкрив Ghost Rider:

- `RWS::FileSystem` (Головний клас файлової системи)
- `RWS::FileSystem::CArchiveManager` (Керує ARC-архівами)
- `RWS::FileSystem::CArchive` (Представляє один змонтований `.ARC` архів)

**Методи CArchiveManager:**

- `FindAchive(char const*)`
- `Mount(char const*)`
- `FindEntry(char const*, RWS::FileSystem::CArchive**)`

Також є `MemoryFileSystem` (можливо для стрімінгу у пам'ять) та `AssetTracker`. Це підтверджує, що `RTFSSIOP.IRX` лише постачає сирі байти для `RWS::FileSystem::CArchiveManager`.

### 2. Audio & RWA

Ми знайшли простори імен `Audio::` та `RWS::`:

- `Audio::CAudioRelay` (Керує чергами аудіо)
- `RWS::RwsAudio::AddDictionary(RwaWaveDict*)`
- `RWS::RwsAudio::AllocateVirtualVoice()`
- `RWS::RwsAudio::FadeEnvironment(int, unsigned int)`

Ці символи показують, що RenderWare Audio (`RWA.IRX`) тісно спілкується з класом `RwsAudio` на стороні EE, використовуючи "Virtual Voices".

### 3. Engine Core

Видно сотні методів з префіксом `ClimaxP1...` (що підтверджує внутрішню назву рушія "Climax P1"):

- `ClimaxP1AtomicDataReadStream()`
- `ClimaxP1DictionarySchemaCreateChainGraphFromDict()`

## What does this mean for our Toolkit?

Знаючи точну архітектуру рушія, ми можемо перейменувати наші власні класи в Тулкіті (`Loader.cpp`, `Arc.cpp`), щоб вони відповідали оригінальній архітектурі Climax Engine (`RWS::FileSystem::CArchiveManager`, `RWS::FileSystem::CArchive` і т.д.). Це зробить наш "Native Port" (реверс-версію) максимально автентичним оригінальному коду!
