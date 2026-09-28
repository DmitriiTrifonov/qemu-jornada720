# patches

`qemu-src/` — рабочая копия апстримного QEMU, в git этого репозитория не
попадает (см. `.gitignore`). Но пара файлов там (`hw/arm/Kconfig`,
`hw/arm/meson.build`) правится вручную, чтобы подключить наш
`jornada720.c` к сборке — эти правки нужно применять заново при
переклонировании `qemu-src`.

Применить после клонирования qemu-src (после того как также создан
симлинк `qemu-src/hw/arm/jornada720.c -> ../../../src/hw/arm/jornada720.c`,
см. `docs/plan.md`):

```
cd qemu-src
git apply ../patches/0001-wire-up-jornada720-machine.patch
git apply ../patches/0002-sa1110-aux-control-register-dummy.patch
git apply ../patches/0003-fix-sssr-bit-positions.patch
git apply ../patches/0004-strongarm-sctlr-suppress-tb-end.patch
git apply ../patches/0005-strongarm-nv-condition-is-nop.patch
git apply ../patches/0006-sa1110-ssp-msb-justified-tx.patch
git apply ../patches/0007-sdl2-no-double-scaling-of-logical-coords.patch
git apply ../patches/0008-sa1110-c15-idle-wfi.patch
```

- `0001-...` — подключает `jornada720.c` к сборке (Kconfig + meson.build).
- `0002-...` — правка `target/arm/helper.c`: добавляет SA-1110-специфичный
  регистр CP15 `c1,c1,0` (Auxiliary Control Register) как RAZ/WI в таблицу
  `strongarm_cp_reginfo`. Без этого реальный ROM Jornada 720 падает в
  Undefined Instruction на первых же сотнях инструкций загрузки (см.
  `docs/research.md`, раздел "Прогон реального ROM").
- `0003-...` — правка `hw/arm/strongarm.c`: исправляет позиции битов SSSR
  (SSP Status Register) на настоящие из SA-1110 Developer's Manual —
  в QEMU они были скопированы из PXA25x и сдвинуты на 1 бит. Без этого
  boot-код Jornada 720 виснет навсегда на poll'е SSP-статуса при общении
  с Epson-дисплеем (см. `docs/research.md`, раздел про баг в SSSR).
  Затрагивает общий код, используемый и другими платами (например
  `collie`) — потенциально более правильно для всех них, но не проверено
  на регрессии.
- `0004-...` — правка `target/arm/helper.c` (применять после `0002`, тот же
  файл): для `ARM_FEATURE_STRONGARM` ставит `ARM_CP_SUPPRESS_TB_END` на запись
  SCTLR, чтобы инструкция сразу после включения MMU ещё исполнялась по старому
  отображению (классическая идиома MMU-enable, на которую полагается ROM).
  Без этого — Prefetch Abort сразу после включения MMU (см. `docs/research.md`,
  разделы про MMU-enable). Не проверено на регрессии для `collie`.
- `0005-...` — правка `target/arm/tcg/translate.c`: на StrongARM инструкции
  с условием NV (`cond=0xF`) — NOP, а не UNDEF, как на реальном SA-1110. На
  этом держится диспетчер исключений WinCE: без фикса первый же Prefetch
  Abort, который не разрешается через `LoadPageTable`, приводит к вечному
  Data Abort на `0xffff5070` (см. `docs/research.md`). Не проверено на
  регрессии для `collie`.
- `0006-...` — правка `hw/arm/strongarm.c` (применять после `0003`, тот же
  файл): при 8-битном кадре SSP берёт передаваемый байт из старшей половины
  16-битного SSDR, если он записан туда. И CE-ROM, и Linux
  (`jornada720_ssp.c`) пишут в SSDR `byte << 8`; без правки QEMU обрезал это
  до нуля, и MCU клавиатуры/тачскрина ничего не получал. По мануалу SA-1110
  не сверено (bitsavers был недоступен). Не проверено на регрессии для `collie`.
- `0007-...` — правка `ui/sdl2.c`: SDL2-рендер задаёт логический размер
  (`SDL_RenderSetLogicalSize`), и SDL уже сам переводит координаты мыши и
  касаний в логические. QEMU масштабировал их повторно по размеру окна, и
  в окне больше экрана гостя (на весь экран телефона) касания съезжали к
  левому верхнему углу. Теперь при заданном логическом размере координаты
  не пересчитываются. Сборка с SDL: `meson setup --reconfigure build
  qemu-src -Dsdl=enabled` (нужен `sdl2-dev`).
- `0008-...` — правка `target/arm/helper.c` (после `0002` и `0004`): для
  StrongARM `c15,c8,2` — ждать прерывания (WFI), `c15,c1,2`/`c15,c2,2`
  (переключение тактов) — пустые операции. Так ядро CE уходит в простой.
  Заметного эффекта пока нет: CE и «в простое» постоянно чем-то занят.
