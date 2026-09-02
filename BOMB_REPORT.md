# Отчёт по Binary Bomb

## Исследованный бинарь

`~/bomb_hard` — динамически связанный ELF64 x86-64 без PIE и DWARF debug info. Таблица символов не удалена, поэтому доступны имена `phase_1`–`phase_4`, `fib`, `secret_phase` и `explode_bomb`.

SHA-256:

```text
6eeac85d4894be5cb85c5f8e793cd8b8ad884c2490652820b2ed7b2070ad4bbd
```

## Ответы

```text
Phase 1: Rust is blazingly fast and memory-efficient
Phase 2: 1 2 4 8 16 32
Phase 3: 0 452
Phase 4: 10 blazingpower
Secret phase: 1337
```

## Ход анализа

### Phase 1

`phase_1` вызывает `strcmp` со строкой по адресу `0x402069`. Инструкции `cmp $0, %eax` и `je` пропускают `explode_bomb` только при точном совпадении строк.

### Phase 2

`sscanf` должен вернуть `6`, затем первое число сравнивается с `1`. В цикле предыдущее значение удваивается через `shl $1, %ecx`; `cmp %ecx, %eax` и `je` требуют, чтобы каждое следующее число было вдвое больше предыдущего.

### Phase 3

`sscanf` читает два целых числа. Индекс должен находиться в диапазоне `0..5`; после проверки выполняется косвенный `jmp` через таблицу по адресу `0x402008`.

| Индекс | Требуемое второе число | Адрес ветки |
|---:|---:|---:|
| 0 | 452 (`0x1c4`) | `0x401326` |
| 1 | 107 (`0x6b`) | `0x40133d` |
| 2 | 901 (`0x385`) | `0x401351` |
| 3 | 212 (`0xd4`) | `0x401368` |
| 4 | 713 (`0x2c9`) | `0x40137f` |
| 5 | 888 (`0x378`) | `0x401396` |

Для проверки выбран первый допустимый вариант: `0 452`.

### Phase 4

Формат по адресу `0x4020da` — `%d %19s`. Первое число проверяется signed-переходами `jl` и `jle`, поэтому допустим диапазон `0..12`. Затем число передаётся в рекурсивную `fib`, а результат сравнивается с `0x37`, то есть `55`. Условие выполняется для `F(10)`.

После прохождения числовой проверки `strcmp` сравнивает второй токен со строкой по адресу `0x4020f3`. Совпадение заставляет `phase_4` вернуть `1`; `main` воспринимает это как условие входа в `secret_phase`.

### Secret phase

`secret_phase` читает целое число через `sscanf` и сравнивает его с `0x539`, то есть `1337`.

## Использованные команды

Вне GDB:

```sh
file ~/bomb_hard
sha256sum ~/bomb_hard
strings -a -t x ~/bomb_hard
nm -n --defined-only ~/bomb_hard
readelf -hW ~/bomb_hard
readelf -lW ~/bomb_hard
objdump -d --disassemble=phase_1 ~/bomb_hard
objdump -d --disassemble=phase_2 ~/bomb_hard
objdump -d --disassemble=phase_3 ~/bomb_hard
objdump -d --disassemble=fib ~/bomb_hard
objdump -d --disassemble=phase_4 ~/bomb_hard
objdump -d --disassemble=secret_phase ~/bomb_hard
```

В GDB:

```text
set disassembly-flavor att
disassemble phase_1
disassemble phase_2
disassemble phase_3
disassemble fib
disassemble phase_4
disassemble secret_phase
break explode_bomb
break phase_1
break phase_2
break phase_3
break phase_4
break secret_phase
info breakpoints
x/s 0x402069
x/6gx 0x402008
x/s 0x4020da
x/s 0x4020f3
```

Breakpoints разрешились в следующие адреса:

```text
explode_bomb  0x401184
phase_1       0x4011b4
phase_2       0x4011f4
phase_3       0x4012d4
phase_4       0x401444
secret_phase  0x4014e4
```

## Проверка результата

GDB прочитал символы, дизассемблировал функции, показал строки и таблицу переходов. Запуск inferior под GDB на сервере запрещён политикой `ptrace` и завершался сообщением `Operation not permitted`; ограничение не обходилось.

Рассчитанный ввод проверен обычным запуском `~/bomb_hard`. Все четыре основные фазы завершились сообщением `passed`, после чего была обнаружена и успешно обезврежена скрытая фаза. `explode_bomb` не вызывался.
