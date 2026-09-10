# CT07 — мастер-план переработки ядра и дерева устройства

Дата: 2026-07-06, обновлено 2026-07-09. Статус: **DEVICE-FREE WORK
IMPLEMENTED; LIVE GATES PENDING**.
Автор: Claude (аудит всего дерева `/srv/forge/android/ctyon` + всех 3018 строк
`BRINGUP_STATE.md` + истории kernel-репозитория).
For agents: this is the master rework plan for the CT07 bring-up; execute in
the order of section 10; evidence rules of `AGENTS.md`/`CLAUDE.md` apply.

---

## Обновление исполнения 2026-07-09

Снимки HEAD/конфига/артефактов ниже сохранены как история исходного плана.
Текущее состояние после полного повторного аудита:

- скрипт сборки фиксирует toolchain, точное состояние source, build log и
  манифест; опасные/reused build ID запрещены;
- `ct07_defconfig` и `ct07_bringup_defconfig` разделены: production возвращает
  KPOC и suspend, диагностические трассировки/USB force/heartbeat остаются
  только в bring-up; штатный WDT включён в обоих профилях;
- в обоих профилях удалены доказанные фантомные драйверы (нештатные
  ALS/gyro/magnetometer/NFC/touch/lens/Pump Express), восстановлены stock
  Passpoint/C2K/SBP флаги;
- DA226/MIR3DA добавлен и выровнен с CT07 DT (`mediatek,gsensor`, I2C `0x26`,
  семь base attrs); отдельным риском остаётся частный stock factory-ioctl ABI;
- SP0A09 power wiring, PDN polarity, I2C speed, mode constants, Bayer order,
  init table, stream/test-pattern paths и rail cleanup выровнены по точному
  CT07 `vmlinux.elf`;
- диагностический p8 образ имеет уникальный marker, host-verified упаковку и
  600-секундный emergency timer; аппаратный WDT остаётся последней защитой
  при зависании. Фактический возврат в p7 всё равно является live gate.

Ни один новый бинарник этого состояния ещё не прошивался. Gate G1, USB,
экран, DA226, камера, modem/RIL, питание, suspend/resume и длительная
стабильность остаются аппаратными проверками; точная матрица ведётся в
верхнем `docs/KERNEL_READINESS_CT07.md`.

---

## 0. Цель

1. **Рабочее, воспроизводимо собираемое ядро CT07 + чистое дерево устройства**,
   пригодные как база для сборки новых версий Android из исходников.
2. **Постоянная инфраструктура ранней диагностики падений** вместо разовых
   хаков (прямой запрос пользователя).
3. **Решение по эксперименту Android 7 (CALME)**: графт-порт сворачивается до
   диагностического инструмента; целевой путь — сборка ROM из исходников.

---

## 1. Точка отсчёта: что установлено

### Устройство (все FACT; источники: `BRINGUP_STATE.md`, `firmware_unpacked/mounts/system/build.prop`, `reverse_engineering_status.xml`)

- Bird CT07 (FinePower CT07 / BenWee M1) — **кнопочный телефон, БЕЗ Volume-Up**:
  вход в recovery по кнопкам невозможен (BRINGUP_STATE.md:2487).
- SoC MT6737T (семейство MT6735), 32-bit Cortex-A53, PMIC **MT6328** (не MT6323),
  зарядник — линейный, встроенный в PMIC.
- Дисплей **NV3029G** QVGA 240×320, SPI-транспорт (LK различает панели по ID:
  0x3033=NV3029G / 0x9341=ILI9341; cmdline стокового LK называет NV3029G —
  на этом экземпляре стоит NV3029G). Камера SP0A09 MIPI. Клавиатура mtk-kpd.
- Сток: Android 6.0 (SDK 23, сборка 2018-07-16), ядро 3.18.19.
- Разделы: p7=boot (16M), p8=recovery (16M), p10=expdb, p20=system (1.29G);
  GPT — в `ctyon-mt6735.zip` (gpt.bin).
- RAM: **НЕ ПОДТВЕРЖДЕНО.** DTS memory node = 2GB (reg 0x80000000), но LK
  правит memory node на лету → проверить `/proc/meminfo` в Фазе 1.

### Репозиторий ядра (FACT)

`kernel/mediatek/mt6735`, ветка `kernel_ctyon_ct07` (= origin):
донорский BSP ALPS 3.18.19 (`chelghouf/ALPS-MP-M0.MP1-...VZ6737M...`) + 4 коммита:

| SHA | Что сделано |
|---|---|
| `4a7d102e` | стоковый DTS (байт-в-байт со стоковым DTB — проверено на 3 уровнях), appended-DTB конфиги |
| `abe10928` | `ct07_defconfig`, LCM (ct07_lcm_spi, ili9341, nv3029g) и камера SP0A09, **WDT hard-off**, **KPOC off** + override `get_boot_mode()`, restart tracer, ранний `set_usb_rdy()` |
| `c586d209` | `is_ready=1` в `musb_gadget_start()` — по Ghidra-RE стока; кандидат root cause USB-deadlock |
| `c5549495` (HEAD) | `pm_suspend()` → no-op, безусловный WDT-kicker |

### Главный вывод журнала (FACT)

**«Ядро даже не стартует» — миф наблюдаемости.** Исходное ядро достигает
userspace (~9.4 с: PID1 init, процесс recovery запускается) — доказано 64 КБ
last_kmsg. Реальные «убийцы», из-за которых аппарат выглядел мёртвым:

1. **KPOC** (Kernel Power Off Charging) при питании от стендового USB:
   форсирует фейковый MTP 0E8D:2008, блокирует adb, ребутит на ~157 с.
2. **HW watchdog MTK** — cold reset на ~35–47 с; cold reset стирает DRAM,
   поэтому логи «исчезали» (источник многократных ложных диагнозов).
3. **pm_suspend/автосуспенд** на ~10–12 с — чёрный экран, USB отваливается,
   «труп». Этот фикс жил только в /tmp-копии и не был закоммичен до
   `c5549495` — прямая причина последнего «ядро не стартует».

Все три нейтрализованы в HEAD. **Но сборка из коммитнутого дерева
(`zImage-dtb` SHA `03986251…`) ещё НЕ прошивалась и НЕ проверялась на
устройстве** — это первый шаг плана.

### Незакрытые блокеры (FACT)

- **USB/adb-энумерация**: цепочка зарядка→gadget идентична стоку; root-cause
  кандидат (`is_ready` в `musb_gadget_start`) исправлен, но не проверен живьём;
  глубже подозревается OTG/B-session state machine (devctl=0x99, pullup не
  взводится).
- **Дисплей NV3029G чёрный** в recovery (драйверная часть по RE идентична
  стоку; suspend как одна из причин уже заблокирован).
- **Стена наблюдаемости ~10.6 с**: ram_console замолкает при старте recovery.

### Состояние самого устройства — НЕИЗВЕСТНО (INFERENCE по журналу)

p7 = CALME n32-022 (висит на логотипе, adb unauthorized), p8 = TWRP
(восстановлен внешним флешером), p20 = система CALME Platinum. Требуется
readback в Фазе 1. Подготовленный n32-023 так и не прошит.

### Гигиена (FACT)

- Верхнеуровневый `/srv/forge/android/ctyon/.git` — **пустой каталог**, истории
  нет; под VCS только ядро.
- `docs/`, `device/`, `vendor/` принадлежат `valakas`, пользователю `n8n`
  недоступны на запись (поэтому этот план лежит в kernel-репо).
- Сборки велись **тремя** тулчейнами (NDK GCC 4.8, crdroid14 GCC 4.9, следы
  host GCC 12), in-source (мусор в дереве), без единого скрипта; артефакты
  жили в /tmp и терялись при ребуте.
- `device/bird/` — по собственному README «provisional scaffold, NOT a
  finished flashable device tree»; `vendor/ctyon/ct07/proprietary/` — пуст
  (блобы не извлечены).
- Гайды `porting` (cp1251, Spreadtrum-туториал) и `port_full_guide.txt`
  (Blackview P2) — чужие generic-инструкции, а не план для CT07.

---

## 2. Принципы (выжимка из горького опыта журнала)

1. **Один канонический тулчейн, out-of-tree сборка, манифест SHA** на каждый
   артефакт. Ничего в /tmp.
2. **Ни одного вывода по несвежему логу**: каждый прошиваемый образ несёт
   уникальный nonce в cmdline; cold reset стирает DRAM (pstore выживает только
   после warm reset) — правило свежести обязательно.
3. **Все bring-up-хаки — под конфигом/cmdline**, а не правкой прод-поведения
   (сейчас WDT/KPOC/suspend отключены безусловно — так шипить нельзя).
4. **Fallback-дисциплина**: всегда остаётся хотя бы один проверенный
   загружаемый слот (p7 или p8) + SPFT как последний рубеж. Никогда не шить
   оба слота за одну итерацию.
5. Каждая рабочая сессия заканчивается коммитом + записью в `BRINGUP_STATE.md`.

---

## 3. Фаза 0 — Гигиена и воспроизводимость (без устройства, ~0.5–1 день)

- **0.1 Права и VCS верхнего уровня**: `sudo chown -R n8n:n8n
  /srv/forge/android/ctyon` (только по подтверждению пользователя!), затем
  `git init` + `.gitignore` (ctyon-mt6735.zip, out/, firmware_unpacked/,
  kernel/ — свой репо, *.img, крупные бинарники kernel-reverse/) и первый
  коммит docs/device/vendor/scripts/tools/BRINGUP_STATE.md.
- **0.2 `scripts/build_ct07_kernel.sh`**: pinned тулчейн
  `arm-linux-androideabi-4.9` (crdroid14 prebuilts), `make O=<out>
  ct07_defconfig && make O=<out> -j$(nproc) zImage-dtb`; на выходе — манифест
  (SHA zImage-dtb / vmlinux / System.map / .config / DTB, git SHA, дата) в
  `docs/builds/<id>.json`; артефакты в постоянный каталог, не в /tmp.
- **0.3 Чистка in-source мусора** в дереве ядра (только перечисленное:
  `Module.symvers`, `arch/arm/include/generated/`, `sha256-core.S`,
  `.vz6737t*.tmp`, старый `vz6737t_35g_a_m0.dtb`) — сначала `git clean -n`.
- **0.4** `compiler-gcc12.h` оставить как host-шим, задокументировав: канон —
  GCC 4.9.

**DoD:** два подряд запуска скрипта из чистого клона дают идентичные SHA;
манифест записан.

---

## 4. Фаза 1 — Ground state устройства и верификация HEAD (первая сессия с устройством)

- **1.1 Фактическое состояние**: попасть в recovery (см. 1.2), readback SHA
  p7/p8/p20, `/proc/meminfo` (закрыть вопрос RAM), `/proc/cmdline`.
- **1.2 Надёжный вход в recovery без Vol-Up** (критическая инфраструктура
  кнопочника):
  1. из TWRP/adb: запись `boot-recovery` в misc (найти misc по gpt.bin,
     вероятно p5/p6) — протестировать и задокументировать;
  2. `adb reboot recovery` — когда adb жив;
  3. SP Flash Tool (download mode с выключенного) — задокументировать как
     последний рубеж (и путь отката).
- **1.3 Сборка HEAD** скриптом 0.2. Канонический артефакт (Фаза 0 закрыта
  2026-07-06, двойная сборка воспроизводима): `zImage-dtb` SHA
  `157816fe8ac9ae433fc73c19b1494ae3f8c182860cfac2130038674a71d2a652`,
  манифест `docs/builds/repro-a2.json`. (Июньский SHA `03986251…` не
  бит-сравним: реальные user/host/timestamp и in-source comp_dir; конфиг
  идентичен `6994bc8f…`.) Repack с рамдиском TWRP 3.7 (лучший на диске —
  сборка `eng.n8n.20260610.102354`, см. журнал 2026-07-06), маркер
  `androidboot.ct07src=fix2-<nonce>`; **прошить только p8**, p7 не трогать.
- **1.4 Свидетельства**: warm-метод last_kmsg, expdb-маркеры, аптайм.

**Gate G1:** ядро из коммитнутого дерева бутится в recovery и живёт ≥5 минут
(раньше это умели только потерянные /tmp-копии). Провал G1 → приоритет Фазе 2.

Verification commands (для следующей capture-сессии):
```
adb -H 127.0.0.1 -P 15038 -s 0123456789ABCDEF shell 'uname -a; uptime; cat /proc/cmdline'
adb ... shell 'dd if=/dev/block/mmcblk0p10 bs=512 skip=20352 count=1 2>/dev/null | strings'
adb ... shell 'cat /proc/last_kmsg 2>/dev/null | tail -100'   # или /sys/fs/pstore
adb ... exec-out 'cat /dev/block/mmcblk0p8' | sha256sum        # readback только на хосте
```

---

## 5. Фаза 2 — Постоянная ранняя диагностика (стартует сразу, живёт всегда)

Каналы по фазам загрузки (от самых ранних):

- **2.1 UART-консоль — максимальный выигрыш** (HYPOTHESIS: пады на плате есть;
  falsification — вскрыть и прозвонить). Сток уже настроен:
  `console=ttyMT0,921600n1`; стоковый DTS: uart2 RX/TX = GPIO54/55 (в донорском
  DTS были 57/58 — одна из причин «немых» логов, исправлено портом стокового
  DTS). Достаточно RX-only тапа на TX-пад, адаптер 1.8V. Даёт Preloader/LK/
  kernel-лог ДО каких-либо DRAM-логов — то самое «снятие ранней диагностики».
- **2.2 Дисциплина pstore/ramoops** (механика уже понята): warm
  `kernel_restart` сохраняет DRAM, cold WDT reset — стирает; ram_console
  @0x43f00000, pstore @0x43f10000 (DTS). Правила свежести — в runbook.
- **2.3 AEE/ipanic → expdb (p10)** — стоковый канал аварий, переживает cold
  reset: проверить/включить `CONFIG_MTK_AEE_*` в ct07_defconfig, чтобы паники
  падали в expdb (поздние стадии; для до-9с падений не работает — там UART).
- **2.4 Секторные маркеры expdb @0x9f0000** (механизм n32-023) —
  стандартизировать: скрипт стадий init в ramdisk + kernel panic-notifier
  (одна строка, cmdline-gated) + nonce каждого образа.
- **2.5 Стена 10.6 с — РЕШЕНО (2026-07-09).** Причина (FACT): рамдиск TWRP,
  `init.rc:15` = `write /proc/sys/kernel/printk "1 1 1 1"` на `on init` →
  console_loglevel=1 с ~10с, ram_console (зарегистрированная консоль) молчит.
  Обход: `ignore_loglevel` в cmdline (образ fix2a002+), рамдиск не трогаем.
  (Старый текст ниже оставлен как история.) Тест-дискриминатор: контрольный
  boot стокового ядра — идёт ли ЕГО ram_console дальше 10.6 с. Фикс возвращает
  видимость userspace-стадий без
  UART.
- **2.6 Runbook `DIAGNOSTICS_CT07.md`**: таблица «канал → фаза загрузки → как
  читать → правила свежести → команды».

**DoD:** любой неудачный boot оставляет ≥1 свежий (nonce-датированный)
артефакт, по которому определяется стадия смерти.

---

## 6. Фаза 3 — Продуктизация ядра (после G1)

- **3.1 `CONFIG_CT07_BRINGUP` — ВЫПОЛНЕНО 2026-07-09.** Под флагом оставлены
  KPOC-override, pm_suspend-block, restart tracer, USB-форсы и диагностический
  2-секундный kicker. Production-профиль возвращает KPOC и suspend. Старый
  пункт про `WDT-off` отменён: аппаратный WDT включён в обоих профилях.
- **3.2 WDT — ПЕРЕПРОВЕРЕНО И ВКЛЮЧЕНО В ОБОИХ ПРОФИЛЯХ.** RE выполнен 2026-07-09
  (`kernel-reverse/wdt-stock-vs-source-20260709.md`). Гипотеза «разница
  конфигурации wd_kicker/таймаута» **ОТВЕРГНУТА**: сток и источник настраивают
  WDT побитово одинаково (probe→enable, 30с, dual-mode, kicker `wdtk-N` каждые
  20с; live-dmesg стока: кик каждые 20с до 429с при взведённом WDT). Реальная
  причина (INFERENCE): на старом источнике kicker переставал планироваться в
  окне ~10–17с (то же, что suspend/KPOC), WDT оставался взведён → cold reset
  +30с = 35–47с. Реализованный фикс удаляет оба форс-отключения из
  `mtk_wdt.c`; `CONFIG_MTK_WATCHDOG=y`, `CONFIG_MTK_WD_KICKER=y` и штатный
  таймаут сохранены. Дополнительный 2-секундный `ct07wdt` и 600-секундный
  emergency timer существуют только в bring-up; production их не содержит.
  Старое указание поместить `wdt_en=FALSE`/`WK_WDT_DIS` под bring-up-флаг
  **явно отменено**: его возврат снова создаст непредсказуемый чёрный экран без
  аппаратного fallback.
- **3.2a Открытый вопрос — инициатор suspend.** Что вообще запускает suspend на
  источнике, НЕИЗВЕСТНО (в рамдиске нет писателя `/sys/power/*`, в дереве нет
  вызова pm_suspend). Добавлен одноразовый `dump_stack()` в `pm_suspend()`
  (commit 76bfdaf5) → на первом буте caller осядет в pstore (ловить с
  `ignore_loglevel`). Это разблокирует корректный возврат suspend в 3.4.
- **3.3 KPOC вернуть** в прод-профиле (зарядка выключенного аппарата — базовая
  функция телефона); стендовый bring-up остаётся без KPOC.
- **3.4 pm_suspend вернуть**; удержание бодрствования в recovery — штатным
  wakelock/настройкой TWRP, не глобальным no-op.
- **3.5 Латентный баг из RE**: NULL-guard в диспетчере `charging_func[]`
  (`charging_hw_pmic.c:696` — индексы 22–31 NULL без проверки).
- **3.6 Два defconfig**: `ct07_defconfig` (прод) / `ct07_bringup_defconfig`.
  Наследие имён `vz6737t_35g_a_m0` задокументировать; `CONFIG_ARCH_MTK_PROJECT`
  НЕ переименовывать (на нём codegen-пути).
- **3.7 История**: поверх донорской базы собрать тематическую серию (dts /
  drivers / diagnostics / fixes) в новой ветке `ct07-v2` + тег на текущую;
  без force-push в существующую ветку.

**DoD:** прод-сборка (WDT on, KPOC on, suspend on) бутится и живёт;
bringup-сборка сохраняет всю диагностику.

---

## 7. Фаза 4 — Остаточные аппаратные блокеры

- **4.0 Метод: RE-выравнивание со стоком (сток = ground truth).**
  Подтверждённая методика — `is_ready`-фикс (`c586d209`) найден именно
  сравнением декомпилята стока с исходником. Скрининг ВСЕГО ядра по карте
  символов выполнен 2026-07-06 (артефакты и методика:
  `kernel-reverse/symdiff-20260706/` в проектном репо): пропущенных
  низкоуровневых подсистем НЕТ — SPM/MTCMOS, DCM, cpufreq/hps, EMI, clk,
  PMIC, thermal, GPU (kbase/GED), DDP/CMDQ, msdc, ccci присутствуют, счётчики
  функций в пределах откалиброванного шума (инлайнинг + typing-артефакт
  kallsyms); низкоуровневый board-конфиг уже стоковый через байт-в-байт DTS.
  Донор ALPS M0.MP1 V2.55.6 и сток V2.84 — одна ветка BSP. Оставшиеся
  расхождения — ПОВЕДЕНЧЕСКИЕ (init-значения, ветки, тайминги), символьный
  скрининг их не видит: ищем прицельным decompile-compare (GhidraMCP,
  стоковый `vmlinux.elf`) по подсистеме активного блокера, а не сплошным
  портированием.

- **4.1 USB/adb** (первым — дешёвая проверка): G1-образ уже несёт `is_ready`-
  фикс — проверить энумерацию. Если мертво: kernel-тред (ct07wdt уже живёт)
  периодически пишет MUSB DEVCTL/POWER/INTRUSB + charger_type в
  ram_console/expdb → warm-capture; Ghidra-сравнение init USB PHY
  (U2PHYDTM0/1, session-биты) сток vs источник; форс b_peripheral. Все форсы —
  cmdline-gated. **DoD: `adb devices` видит аппарат по USB.**
- **4.2 Дисплей**: LK рисует лого (FACT из CALME-циклов «висит на логотипе») →
  панель и её LK-инициализация живы; после блокировки suspend проверить
  dmesg (`CT07_LCM_SPI`, DISP/DDP), backlight (disp_pwm/leds), тест-паттерн в
  `/dev/graphics/fb0`; сверить kernel-DDP init с параметрами LK.
  **DoD: TWRP UI виден на экране.**
- **4.3 Клавиатура**: стоковая матрица уже в DTS; проверить event-поток и
  `ct07-keymouse` на исходном ядре.
- **4.4 Далее по одному**: зарядка (штатный KPOC-путь), аудио, модем/RIL — с
  гейтом и откатом на каждом шаге.

---

## 8. Фаза 5 — Android 7 (CALME) и «новые версии Android»

### Честная оценка прежнего подхода

- Порт вёлся по чужим generic-гайдам (`porting` — cp1251-туториал по
  Spreadtrum-recovery; `port_full_guide.txt` — чеклист Blackview P2).
- Свап 195 вендор-библиотек A6 в систему A7 дал предсказуемые missing-symbol
  крахи (FACT, журнал 2026-06-18); SELinux чинился хаками (`setenforce 0`,
  чужой `file_contexts`).
- 64-bit донор CALME HERO на 32-bit ядре — ABI-невозможен (FACT журнала),
  отвергнут правильно.
- Графт на **стоковом** ядре в принципе не решает задачу «собирать новые
  версии Android».

### Решение

- **Track A (диагностический, 1–2 сессии):** прошить уже готовый n32-023
  ТОЛЬКО как носитель expdb-стадийных маркеров (заодно валидация Фазы 2) и
  получить ответ «на какой стадии умирает A7-графт». Дальше графт **не
  развивать**.
- **Track B (целевой): сборка ROM из исходников** — LineageOS 14.1 / OmniROM
  (Android 7.1, 32-bit arm, ядро 3.18 совместимо; TWRP уже собирался как
  `omni_ct07-eng` из соседнего дерева `twrp51-m681` — база инфраструктуры
  есть). Работы:
  1. довести `device/` (сейчас scaffold) до полного дерева: BoardConfig
     (частично готов, `TARGET_KERNEL_CONFIG := ct07_defconfig`), fstab/init из
     стока, sepolicy на базе device/mediatek-шаблонов;
  2. `vendor/`: извлечь минимальный проверенный набор блобов (из журнала:
     audio.primary, gralloc, hwcomposer, camera, sensors, lights, memtrack,
     gps + RF firmware + mddb) от стока A6; при несовместимости с A7 —
     одноимённые от донора CALME Platinum (сам MT6737M/A7);
  3. ядро — inline-сборка из нашего репо.
  Milestones: (1) boot-to-UI, (2) RIL/звонки, (3) камера/аудио, (4) зарядка.
  Предусловие: Фаза 4 закрыта (USB + дисплей).
- Ресурсы: подтвердить RAM в Фазе 1; при 512MB LOS 14.1 для QVGA остаётся
  реалистичным (lowram-флаги, без GApps).

---

## 9. Риски и неизвестные

| Риск / неизвестное | Митигция |
|---|---|
| UART-падов может не оказаться | Фазы 2.2–2.5 дают покрытие без UART |
| LK закрыт (только lk.bin) | RE по мере надобности (Ghidra), LK не трогаем |
| Один аппарат, нет fastboot, кнопочник | fallback-дисциплина слотов + SPFT; полный дамп уже есть (ctyon-mt6735.zip) |
| RAM/размер system (1.29G) для A7 | подтверждение в Фазе 1; обрезка ROM |
| Права valakas/n8n блокируют VCS верхнего уровня | chown по подтверждению пользователя (0.1) |
| adb unauthorized на текущем p7 | вход через misc/SPFT (1.2) |
| `03986251…` может не воспроизвестись | сравнение манифестов до прошивки (1.3) |

---

## 10. Порядок исполнения

1. **Фаза 0** (без устройства) — гигиена, скрипт сборки, воспроизводимость.
2. **Фаза 1 + 2** (первая сессия с устройством) — ground state, вход в
   recovery, прошивка HEAD-сборки, диагностические каналы. → **Gate G1**.
3. **Фаза 4.1 USB** (дешёвая проверка is_ready-фикса) → **4.2 дисплей**.
4. **Track A**: n32-023 попутно (валидация маркеров + стадия смерти графта).
5. **Фаза 3** — продуктизация ядра (прод/bringup-профили, возврат WDT/KPOC/
   suspend).
6. **Track B** — device tree + vendor + сборка LOS 14.1/Omni.

Фаза 2 (диагностика) начинается сразу и поддерживается постоянно.

---

## 11. Patch history

### 2026-07-10 — section-mismatch lifetime repair

**Category: PROPER-FIX.**

**Hypothesis.** The 36 final-production modpost mismatches are not one benign
legacy warning class. Early-only memory scanners merely lack `__init`, while
boot-mode, PMIC, framebuffer, CCCI, ATF, cpuidle, fault-hook and SPI paths can
remain callable after `free_initmem()`. Fixing the actual lifetime boundary,
instead of adding `__ref` or suppressing modpost, must reduce verbose modpost
to zero without changing the production config or compiled DTB.

**Evidence.** Pre-fix artifact
`out/kernel-builds/production-final-20260710/vmlinux` (project-root path),
SHA-256 `759a6e3105c9d246810769ecd38a03835d42b24d196ea2969ddd477912889b1c`,
reported `Found 36 section mismatch(es)`; the complete caller/callee ledger is
`docs/CT07_SECTION_MISMATCH_AUDIT.md` in the project root. A clean production
tmpfs build from the patched tree completed `zImage-dtb`; direct verbose
modpost printed zero lines. Its `.config` SHA-256 remains
`3b26d07209bb1627f24b1caf3180ec51f98d18efc3e611635e219c54ef3af871`
and DTB SHA-256 remains
`699b6b9db6925138fdf063df8430d7a1988841c06b45a8b7ee93184d08d367bd`.

**Files changed and why.**

- `arch/arm/mm/fault.c`, `drivers/spi/mediatek/mt6735/spi-dev.c`, and
  `drivers/misc/mediatek/base/power/spm_v1/mt_idle.c`: keep genuinely
  runtime-callable hooks/probes/helpers resident instead of pointing at freed
  init text.
- `drivers/misc/mediatek/{atf_log,boot,boot_reason,ccci_util,power,video}`:
  replace runtime use of flat-DT init helpers with refcounted live OF-node and
  property lookup, preserving the raw MTK/LK tag layout.
- `drivers/misc/mediatek/base/power/spm_v1/mt_spm_internal.c`: read the
  persistent live memory node, validate `orig_dram_info`, and make repeated
  calls safe.
- `drivers/misc/mediatek/mem/{mtk_memcfg.c,mtk_meminfo.c}`: mark proven
  early-only flat-DT callbacks `__init`; the exported DRAM-size getter no
  longer retries an early-only parser after init memory has been freed.

**Expected next marker.** Final production and bring-up builds must both show
zero verbose modpost output. The next packaged p8 test uses
`androidboot.ct07src=fix2a006`; G1 must retain that marker for at least 300 s.

**Rollback condition.** Revert this commit if a hash-verified `fix2a006` boot
regresses boot-mode/reason, LCM tag parsing, PMIC DLPT, ATF log reservation,
CCCI modem selection, SPM suspend, DRAM size, cpuidle or SPI probe relative to
`fix2a005`, while partition identity and capture freshness are proven.

**Verification commands.** Run `scripts/build_ct07_kernel.sh` from the project
root once with `DEFCONFIG=ct07_defconfig` and once with
`DEFCONFIG=ct07_bringup_defconfig`; then invoke each build's `scripts/mod/modpost`
without `-S` against its exact `vmlinux.o` and require empty output. Package
only the bring-up manifest as `fix2a006`, verify its full 16 MiB SHA/readback,
then run `scripts/capture_ct07_g1.sh fix2a006 <p8-sha> <p7-sha>` on the current
explicit ADB port.

### 2026-09-02 — 39e8861f: disable the BATON battery-presence check (stock alignment)

- **Category:** PROPER-FIX (stock = hardware truth, AGENTS.md rule 6).
- **Hypothesis:** with a USB cable attached the source kernel can power the
  phone off from `battery_common.c`: `check_battery_exist()` (probe of the
  charger-hv workaround + charger-detect paths) → three
  `RGS_BATON_UNDET` reads → `charging_set_power_off()` → `kernel_power_off()`.
  The stock kernel never takes that path. Whether this is the ~150 s
  power-off of 2026-09-01 is a HYPOTHESIS (see ctyon `BRINGUP_STATE.md`
  2026-09-02 for the competing ones: charger-type detection, Tbat ≥ 60).
- **Evidence:** stock TWRP boot log
  `ctyon/out/ct07-live-20260901/rom-attempt-1/last_kmsg.txt:309`
  `[    9.219321] … [BATTERY] Disable check battery exist.`; stock kernel
  binary (`boot-stock.bin`, gzip @16943): "Disable check battery exist" ×1,
  "Battery is not exist, power off" ×0; `out/kernel-builds/bringup-xperms-20260831/vmlinux`
  (84d62559): ×0 / ×1; stock `ProjectConfig.mk:267`
  `MTK_DISABLE_POWER_ON_OFF_VOLTAGE_LIMITATION = no` (the only path that
  defined the macro in this tree), so the vendor defined it directly.
- **Files:** `mach/mt_charging.h` — map Kconfig `CONFIG_CONFIG_DIS_CHECK_BATTERY`
  onto `CONFIG_DIS_CHECK_BATTERY`; `ct07_defconfig`, `ct07_bringup_defconfig`
  — set it (verified in the expanded `.config`).
- **Build:** `bringup-batcheck-20260902`, `zImage-dtb`
  `ba598e1bb2a98279a2dbbb14314abd8d86737d9fa684f680e7404ed5df8c28af`, vmlinux
  strings now the stock pair (1/0).
- **Expected next marker:** `[BATTERY] Disable check battery exist.` at ~9.2 s
  in the `ct07_reboot_after=100` last_kmsg; no "Battery is not exist"; device
  alive past 150 s.
- **Rollback:** revert if the device still powers off at ~150 s and last_kmsg
  names another caller, or if charging/battery behaviour regresses vs stock.
- **Verification:** `grep -n "Disable check battery exist\|Battery is not exist\|charging_set_power_off\|CT07_FAILSAFE" /proc/last_kmsg` in TWRP after the warm reboot.

### 2026-09-02 — 4c870c51 + ccf4e3b3: DRAM ring live under PSTORE, RGU DDR-reserve, boot-stage stamps

Build `bringup-dramres-20260902` (ct07_bringup_defconfig, HEAD ccf4e3b3):
`zImage-dtb` `a059674e946c0c16230caeb86f2a5941167002baf79c83e86428889c8770168c`; image
`out/ct07-flash-kit-20260902/boot/boot-rom-dramres-fs100.img` `015391204e050cd51ddcb3ce74cd7dd3a6164a6b746c0b0a1ecf5d8aa2c8b52a`
(ROM ramdisk `rom2-unpacked/ramdisk` byte-identical, 91-byte cmdline
`bootopt=64S3,32N2,32N2 androidboot.selinux=permissive ct07_reboot_after=100 ignore_loglevel`),
padded-16m `98a0b8fb27131d98bf76132532a54eb832b3fc9d79f40a4150095258edbd8c4d`. Not flashed.

#### 4c870c51 — ram_console: record printk text in the DRAM ring under CONFIG_PSTORE

- **Category:** PROPER-FIX (both defconfigs).
- **Hypothesis:** with `CONFIG_PSTORE=y` the ram console at 0x43F00000 is
  never registered as a console and `sram_log_save()` only forwards to
  `pstore_bconsole_write()`, a no-op until ramoops probes at
  `postcore_initcall`; a kernel dying before that leaves a valid DBGC header
  and an empty ring, and no other zone holds text.
- **Evidence:** `mtk_ram_console.c:274-277`, `:517-518` (pre-patch);
  `fs/pstore/platform.c:404-410` `if (psinfo)`; `fs/pstore/ram.c:686`;
  `ct07_bringup_defconfig:440-447`, `:243-248`; every captured log is a
  stock-kernel session (`docs/run_reports/ct07_kernel_log_channel_analysis.md`
  A1-A4, B5); m5c `0c9418f99` + `f1d4d19d9`, P11 "103 lines in the ring"
  (`docs/run_reports/m5c_kernel_bringup_lessons.md` F3, A3).
- **Files:** `drivers/misc/mediatek/ram_console/mtk_ram_console.c` —
  `ram_console_dram_save()` compiled unconditionally and used by both
  `sram_log_save()` variants (PSTORE variant keeps the bconsole forward);
  new `ram_console_con_write()` as the console `.write` (ring only, so the
  pstore bconsole zone does not get a duplicate that `ramoops_pstore_read()`
  would return as a second CONSOLE record); `register_console()`
  unconditional (`CON_PRINTBUFFER` replays log_buf); NULL guard in
  `aee_sram_fiq_log()`; `/proc/last_kmsg` under `PSTORE_CONSOLE` appends the
  previous boot's ring after the pstore record.
- **Expected next marker:** ring at 0x43F00000 holds
  `Linux version 3.18.19+ (ct07@forge)` + boot log, header `log_size > 0`;
  visible in expdb via LK kedump `SYS_RAMCONSOLE_RAW` after a WDT boot, or
  via `/dev/mem` from a DEVMEM recovery, or in this kernel's own
  `/proc/last_kmsg` ("--- ram console ring (previous boot, log_size N) ---").
- **Rollback:** console-lock hang or duplicated text in `/proc/last_kmsg` on a
  boot that provably reaches userspace; aee WDT/KE dumps missing from the
  bconsole record.
- **Verification:** `dd if=/dev/block/mmcblk0p10 of=/tmp/expdb.bin bs=1048576 count=10`;
  `strings -n 8 /tmp/expdb.bin | grep -c 'ct07@forge\|CT07_STAGE\|CT07_RGU'`;
  `head -1 /proc/last_kmsg`.

#### ccf4e3b3 — ct07: RGU DDR-reserve from console_init + boot-stage stamps in fiq_step

- **Category:** DIAGNOSTIC (`CONFIG_CT07_BRINGUP` only).
- **Hypothesis:** the ~25 s HW WDT reset lets the preloader re-initialise
  DRAM before the next kernel reads it; RGU DDR-reserve (MODE bit 7) keeps
  DRAM in self-refresh across the reset, and a stage byte in the ram console
  header's `fiq_step` is printed by LK (expdb) and by the stock TWRP kernel
  (`/proc/last_kmsg` line 1) without `/dev/mem`. Whether the CT07 preloader
  honours the bit is the HYPOTHESIS under test.
- **Evidence:** `mtk_wdt.c:356-373` `mtk_rgu_dram_reserved()`, sole caller
  `wd_api.c:270-282` on the MRDUMP path (`CONFIG_MTK_AEE_MRDUMP` not set);
  `mt_wdt.h`: MODE +0x0, LENGTH +0x4, RESTART +0x8/key 0x1971,
  `DDR_RESERVE 0x0080`, `KEY 0x22000000`; `vz6737t_35g_a_m0.dts:1647-1651`
  `toprgu@10212000` reg `<0x10212000 0x1000>`; later MODE writers
  (`mtk_wdt_mode_config` bits 0-4,6; `mtk_wdt_enable` bit 0;
  `wdt_arch_reset` clears AUTO_RESTART|IRQ|ENABLE|DUAL) preserve bit 7;
  `wd_api.c:33 .ready = 1` (wd_api kick valid from `kernel_init`, so no
  direct-kick change needed); `mtk_ram_console.h:9-38` AEE steps 4..64;
  m5c `90620e0e4` → `303562a05`; m681 HANDOFF_v44 §0.4 (LENGTH write from
  start_kernel killed the boot; MODE RMW only).
- **Files:** `drivers/watchdog/mediatek/wdt/mt6735/mtk_wdt.c` —
  `ct07_rgu_ddr_reserve()` (lazy `of_iomap` of the RGU + `mtk_rgu_dram_reserved(1)`,
  MODE RMW only, prints `[CT07_RGU] <where>: DDR-reserve on, MTK_WDT_MODE=0x…`),
  as `console_initcall` (stamps fiq_step 0xC0) and re-asserted in
  `core_initcall(mtk_wdt_get_base_addr)`; `init/main.c` — `ct07_stage()`
  stamps 0xC1 kernel_init, 0xC2 pre-SMP, 0xC3 SMP done, 0xD0+level per
  initcall level, 0xE0 before exec of `/init`, each with a `[CT07_STAGE]`
  printk; no-op stubs when the option is off.
- **Not done (R3a/R3b of the lessons report):** single-mode WDT (changes
  crash semantics, irrelevant before the postcore probe) and direct
  `mtk_wdt_restart()` kicks (wd_api already valid).
- **Expected next marker:** stock TWRP after one loop iteration:
  `ram console header, hw_status: 5, fiq step N.` with N ≠ 0 — 192 (0xC0)
  died after console_init, 193-195 kernel_init/pre-SMP/SMP window,
  208+L (0xD0+L) inside initcall level L (0 early … 7 late), 224 (0xE0)
  reached exec of `/init`; same value in `/proc/aed/reboot-reason` and in
  expdb `fiq_step 0x..`. N = 0 with this image verified in p7 ⇒ DRAM did not
  survive (preloader ignores bit 7) or the kernel never reached
  `console_init`.
- **Rollback:** the boot dies earlier than before (fiq step 0, loop period
  < 25 s) ⇒ drop the console_initcall placement; preloader hangs with DRAM
  preserved (no LK, no TWRP on key 8) ⇒ drop DDR-reserve.
- **Verification:** `grep -c 'ct07_rgu_ddr_reserve\|ct07_stage' System.map`;
  on the phone `head -1 /proc/last_kmsg; cat /proc/aed/reboot-reason`;
  `strings -n 6 /tmp/expdb.bin | grep 'fiq_step\|ct07@forge\|CT07_RGU\|CT07_STAGE' | tail -20`.

### 2026-09-02 — 371e7f62: boot stamps must not clobber r1/r2/r0 and must bypass the cache

Build `bringup-mmufix-20260902` (ct07_bringup_defconfig, tree = ec0129f2 +
the diff of 371e7f62, `source.patch` sha256 `3e896986…` == `git diff ec0129f2 371e7f62`):
`zImage-dtb` `64a9589f4692585b25cef3385b97175ff65da3ed43cca1327bd81d5cf067367f`; image
`out/ct07-flash-kit-20260902/boot/boot-rom-mmufix.img`
`8f61655df1ef3b45f8d578737d9d9c6433fd195b185bceead31c512cf2baf6d9`
(ROM ramdisk `rom2-unpacked/ramdisk` byte-identical, 91-byte cmdline as
`recfs100s.json`). Not flashed. Full analysis:
`docs/run_reports/ct07_mmu_turnon_analysis.md` (ctyon repo).

- **Category:** DIAGNOSTIC (`CONFIG_CT07_BRINGUP` only; ct07_defconfig off).
- **Hypothesis:** "the kernel dies in `__enable_mmu`/`__turn_mmu_on`"
  (BRINGUP_STATE 18:50) is an artefact of the stamps. (1) the decompressor's
  `0xA2` stamp at `__enter_kernel` used r0,r1,r2 *after* `mov r1,r7 / mov r2,r8`
  restored the architecture ID and DTB pointer, so every instrumented build
  entered the kernel with r1 = 0, r2 = 0xA2A2A2A2; `__vet_atags` zeroed r2 and
  `setup_arch()` ended in `setup_machine_tags()` → `dump_machine_table()` →
  `while(1)` → WDT. (2) `0xA4/0xA5/0xA6` used r0,r1,r2 that `__mmap_switched`
  still stores (`cr_alignment`, `__machine_arch_type`, `__atags_pointer`).
  (3) the ram-console section in bringup-map used `mm_mmuflags` (0x11c0e,
  write-back), so post-MMU stamps stayed in L1/L2 and the reset dropped them.
  The MMU path itself is instruction-identical to the stock kernel.
- **Evidence:** `bringup-map-20260902/zImage-dtb` +0x9c0..+0x9ec
  (`e3a020a2 … e3a01080 … e2511001 … e3a00000 e1a0f004`); `vmlinux`
  c0bf92e0..c0bf939c (`str r1,[r5]` / `str r2,[r6]` / `strne r0,[r7]` after
  the stamps), c00081f0 `ldr r7,[sl,#8]`; proc_info c08c17a0 (mm 0x11c0e,
  io 0xc02); System.map `swapper_pg_dir c0004000`, `__turn_mmu_on c08c0e10`
  (identity entry pgd[0x408] = 0x40811c0e), `_end c0e922a4`, empty
  `__pv_table`; stock kernel (`boot-stock.bin`, Image sha256 `1c876058…`)
  `__enable_mmu` c00086a4 / `__turn_mmu_on` c09778e0 / TTB setup c001f3e4
  identical.
- **Files:** `arch/arm/boot/compressed/head.S` (`0xA2` → r0,r7,r8),
  `arch/arm/kernel/head-common.S` (`0xA4-0xA6` → r10,r11,r12),
  `arch/arm/kernel/head.S` (frame entry `PROCINFO_IO_MMUFLAGS | PMD_SECT_XN`
  = 0x43f00c12, strongly-ordered), `init/main.c` (`ct07_stage_early()`:
  splatter + DCCIMVAC + dsb; stamps 0xA7 start_kernel, 0xA8 setup_arch,
  0xA9 mm_init, 0xAA init_IRQ, 0xAB time_init, 0xAC before console_init).
- **Why:** (1)/(2) restore the boot-protocol registers; head.S makes the
  post-MMU writes uncacheable so a WDT reset cannot lose them; main.c
  extends the channel to console_init so the real death is localised.
- **Expected next marker:** `/proc/aed/reboot-reason` splattered fields ≥
  `0xa4a4a4a4`; `0xa7..0xac` = start_kernel progress; `0xac` + `fiq step ≥ 192`
  = console_init reached. `0xb4b4b4b4` again = MMU turn-on truly fails.
- **Rollback:** `0xb4b4b4b4` with the p7 hash verified — keep the register
  fix (it is correct regardless), drop the post-MMU stamps and probe the
  handoff state (SCTLR/HCR/SCR) instead of the page tables.
- **Verification:**
  `sha256sum out/kernel-builds/bringup-mmufix-20260902/zImage-dtb` → `64a9589f…`;
  `flash.sh <serial> boot/boot-rom-mmufix.img 7`; loop once; hold `8`;
  `adb shell cat /proc/aed/reboot-reason > aed_reboot_reason.txt`;
  `grep -o '0x[a-f0-9]\{8\}' aed_reboot_reason.txt | sort | uniq -c | sort -rn | head`.

### 2026-09-10 — durable `-fno-pic` in `arch/arm/Makefile`

**Category:** PROPER-FIX (all profiles; the build script's `KCFLAGS` becomes
belt-and-braces).

- **Why:** `ct07_prestart_code_recon.md` ranked the `-fpic` injection of
  `arm-linux-androideabi-4.9` (driver-spec `%{!fno-pic:...: -fpic}`) as the
  primary root cause: orphan `.data.rel*` sections land inside
  `[__bss_start, _end)`, `__mmap_switched` zeroes them, `start_kernel`'s first
  store (`set_task_stack_end_magic(&init_task)`) faults. Until now only
  `scripts/build_ct07_kernel.sh` carried `KCFLAGS=-fno-pic`, so any kernel
  built outside the script (a plain `make`, another CI, an older manifest
  replay) silently returned to the broken layout.
- **Change:** `arch/arm/Makefile` gets `KBUILD_CFLAGS += -fno-pic` with the
  full evidence comment. Last flag wins, and kbuild appends a directory's
  `ccflags-y` after `KBUILD_CFLAGS` (`scripts/Makefile.lib:104`
  `orig_c_flags = $(KBUILD_CFLAGS) $(KBUILD_SUBDIR_CCFLAGS) …`), so
  `arch/arm/boot/compressed` keeps its deliberate `-fpic` (`ccflags-y :=
  -fpic -mno-single-pic-base …`) and the decompressor still relocates its own
  GOT. Verified on `bringup-mmufix-20260902` (built with script `KCFLAGS`
  only): 0 `.data.rel*` sections, `init_task c0c91a10` / `init_mm c0c9d7e0` <
  `__bss_start c0d57314`, 1 stray `add rN,pc,rN` opcode in 3 488 965 words of
  the decompressed image (pre-fix builds: 116 488).
- **Post-link assertion** (ctyon repo `scripts/build_ct07_kernel.sh`): after
  the build, `readelf -SW vmlinux | grep -c 'data\.rel'` must be 0 and
  `init_task`/`init_mm` must sort below `__bss_start`; otherwise the build
  fails before the manifest is written. Positive control: the same check run
  on `bringup-xperms-20260831/vmlinux` reports 4 sections and
  `init_task c0df7670 ≥ __bss_start c0dc5944` — it would have been rejected.
- **Also:** the script now accepts `CT07_CROSS` (e.g. `arm-eabi-` against the
  June `arm-eabi-4.8` tree, recon finding 16) instead of hard-coding the
  androideabi prefix, and the manifest records the real compiler version and
  prefix instead of a hardcoded string. Default behaviour is unchanged.
- **Expected next marker:** unchanged — the `bringup-mmufix-20260902` image
  already contains both the safe stamps and the non-PIC codegen; a fresh
  build from this commit is for the production profile (and for
  reproducibility of the fix itself), not a new hypothesis.
