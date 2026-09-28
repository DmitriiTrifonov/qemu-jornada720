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
