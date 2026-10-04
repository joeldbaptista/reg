# reg

reg is a small 64-bit register machine. It has two programs:

- `regas` assembles a `.reg` source file into a binary.
- `reg` runs a binary and writes the final machine state to a text file.

## Build

	make

The build needs a C99 compiler. Edit `config.mk` to change `CC` or the flags.

## Usage

	regas -i examples/factorial.reg -o fact.bin
	reg -i fact.bin -o fact.out

`fact.out` lists every register, the flags `zf`, `sf` and `of`, and every
static data word.

## Machine

- 16 registers `r0`..`r15`, each 64 bits wide.
- `r7` is the stack pointer. `push`, `pop`, `call` and `return` use it.
- A stack of 4096 words, separate from static data.
- Up to 4096 instructions and 4096 static data words.
- Execution starts at the label `main`.

By convention, `r0` holds the return value and `r5` holds the first argument.

## Language

One statement per line. `#` starts a comment.

| Statement                  | Meaning                                  |
|----------------------------|------------------------------------------|
| `name:`                    | code label                               |
| `rA = 5`                   | load an immediate                        |
| `rA = rB`                  | copy a register                          |
| `rA += rB` / `rA += 5`     | also `-=`, `*=`, `/=`, `%=`              |
| `++rA` / `--rA`            | increment / decrement                    |
| `push rA` / `pop rA`       | stack operations                         |
| `goto L`                   | jump                                     |
| `call L` / `return`        | subroutine call and return               |
| `if rA < rB goto L`        | also `==`, `!=`, `>`, `<=`, `>=`; `rB` may be an immediate |
| `if !rA goto L`            | jump if `rA` is zero                     |
| `if rA && rB goto L`       | also `\|\|`                              |
| `stop`                     | halt                                     |

### Static data

A `static:` label opens a data block. Each line in the block declares one
word. Any later code label closes the block.

	static:
	    count = 0

	main:
	    r0 = *count         # load
	    ++r0
	    *count = r0         # store
	    stop

## Examples

The `examples/` directory contains the following programs (complete list):

- `factorial.reg`: recursive 10!.
- `fibonacci.reg`: iterative fib(10) in registers.
- `fibonacci_static.reg`: iterative fib(20) in static data.
- `globals.reg`: sum of static words.
- `sumsquares.reg`: sum of squares with a callee-saved register.
