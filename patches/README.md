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
cd qemu-src && git apply ../patches/0001-wire-up-jornada720-machine.patch
```
