# Binary Bomb report

## Analyzed binary

`~/bomb_hard` is a dynamically linked ELF64 x86-64 binary without PIE or DWARF debug info. Its symbol table has not been stripped, so the names `phase_1`–`phase_4`, `fib`, `secret_phase`, and `explode_bomb` are available.

SHA-256:

```text
6eeac85d4894be5cb85c5f8e793cd8b8ad884c2490652820b2ed7b2070ad4bbd
```

## Answers

```text
Phase 1: Rust is blazingly fast and memory-efficient
Phase 2: 1 2 4 8 16 32
Phase 3: 0 452
Phase 4: 10 blazingpower
Secret phase: 1337
```

## Analysis steps

### Phase 1

`phase_1` calls `strcmp` with the string at address `0x402069`. The `cmp $0, %eax` and `je` instructions skip `explode_bomb` only when the strings match exactly.

### Phase 2

`sscanf` must return `6`, then the first number is compared with `1`. In the loop, the previous value is doubled using `shl $1, %ecx`; `cmp %ecx, %eax` and `je` require each subsequent number to be twice the previous one.

### Phase 3

`sscanf` reads two integers. The index must be in the range `0..5`; after the check, an indirect `jmp` is performed through the table at address `0x402008`.

| Index | Required second number | Branch address |
|---:|---:|---:|
| 0 | 452 (`0x1c4`) | `0x401326` |
| 1 | 107 (`0x6b`) | `0x40133d` |
| 2 | 901 (`0x385`) | `0x401351` |
| 3 | 212 (`0xd4`) | `0x401368` |
| 4 | 713 (`0x2c9`) | `0x40137f` |
| 5 | 888 (`0x378`) | `0x401396` |

The first valid option was selected for verification: `0 452`.

### Phase 4

The format at address `0x4020da` is `%d %19s`. The first number is checked with the signed branches `jl` and `jle`, so the valid range is `0..12`. The number is then passed to the recursive `fib`, and the result is compared with `0x37`, or `55`. The condition is satisfied for `F(10)`.

After the numeric check passes, `strcmp` compares the second token with the string at address `0x4020f3`. A match causes `phase_4` to return `1`; `main` treats this as the condition for entering `secret_phase`.

### Secret phase

`secret_phase` reads an integer using `sscanf` and compares it with `0x539`, or `1337`.

## Commands used

Outside GDB:

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

In GDB:

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

Breakpoints resolved to the following addresses:

```text
explode_bomb  0x401184
phase_1       0x4011b4
phase_2       0x4011f4
phase_3       0x4012d4
phase_4       0x401444
secret_phase  0x4014e4
```

## Result verification

GDB read the symbols, disassembled the functions, and displayed the strings and jump table. Running the inferior under GDB on the server was prohibited by the `ptrace` policy and failed with `Operation not permitted`; the restriction was not bypassed.

The calculated input was verified by running `~/bomb_hard` normally. All four main phases completed with the message `passed`, after which the hidden phase was discovered and successfully defused. `explode_bomb` was not called.
