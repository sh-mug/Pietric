# Pietric

Pietric is a C++-centric compiler for the Piet programming language. Piet is an esoteric, stack-based language where programs are painted as abstract images. This compiler uses LLVM IR as its intermediate representation and then generates machine code. The Piet stack and I/O are implemented directly in the generated IR, so LLVM's optimizer can see through every operation, and the resulting program needs nothing but the C library (with the POSIX functions `write` and `isatty`).

## Performance

Programs compiled by Pietric run 5 to 80 times as fast as those compiled by [PietCC](https://github.com/pwang00/pietcc), and 150 to 4,900 times as fast as with the [npiet](https://www.bertnase.de/npiet/) interpreter. Each row below is one Piet program with the same input for every implementation; times are in seconds, lower is better.

| Program | **Pietric** | PietCC | npiet | rpiet | piet_programming_language |
|---|---|---|---|---|---|
| Brainfuck interpreter in Piet, running `squares.b` | **0.095** | 5.5 † | 144 | 12.3 | 207 |
| Brainfuck interpreter in Piet, running four nested loops | **0.023** | 0.50 † | 18.3 | 2.17 | 36.8 |
| Sum of c mod 7 for c up to 10⁸ | **0.047** | 3.76 | 231 | 57.5 | > 600 |
| Primes up to 500,000 by trial division | **0.173** | 5.73 | 270 | 68.4 | > 600 |
| Values read 3 to 52 deep in the stack, 10⁷ times | **0.108** | 2.37 | 83.5 | 16.6 | 506 |
| Printing 10⁷ numbers | **0.155** | 0.79 | 24.0 | 7.47 | 240 |

Pietric and PietCC are compilers (the times are those of the compiled programs); [npiet](https://www.bertnase.de/npiet/), [rpiet](https://github.com/tessi/rpiet) and [piet_programming_language](https://github.com/your-diary/piet_programming_language) are interpreters. † PietCC without optimization: with `--o2` it did not compile the program within 600 s. Runs were stopped after 600 s. Measured on an Intel Core i5-12400; see [Benchmarks](#benchmarks) for the details and for how to run them yourself.

## File Structure

```
Pietric/
├── CMakeLists.txt         # CMake build configuration file
├── README.md              # This documentation file
├── .gitignore             # Files/directories ignored by git
├── include/
│   ├── PietTypes.h        # Definitions for Piet colors, DP/CC, and commands
│   ├── Utils.h
│   ├── Parser.h    
│   ├── Graph.h  
│   ├── IRBuilder.h  
│   ├── Runtime.h  
│   └── Backend.h  
├── src/
│   ├── main.cpp        # Command-line driver: parses the options and runs the stages below
│   ├── Utils.cpp       # Utility functions (e.g., hex string conversion)
│   ├── Parser.cpp      # Parses input files (text files with hex codes or BMP/PNG/GIF images)
│   ├── ImageLoader.cpp # Loads images using the stb_image library
│   ├── Graph.cpp       # Builds the execution graph according to Piet’s DP and CC rules
│   ├── IRBuilder.cpp   # Generates LLVM IR from the execution graph, with the stack operations inline
│   ├── Runtime.cpp     # Support functions emitted into the generated module (stack growth, roll, I/O)
│   └── Backend.cpp     # Optimizes the module, generates machine code and links the executable
├── tests/
│   ├── run_tests.sh    # Compiles, runs and checks every program in cases/
│   ├── gen_cases.py    # Generates the text-format test programs in cases/
│   └── cases/          # Test programs with their expected output (and input)
└── bench/
    ├── run.py          # Runs the benchmarks on Pietric and other Piet implementations
    └── programs.py     # Generates the benchmark programs
```

## Building the Compiler

> [!NOTE]
> Make sure that the environment variable `LLVM_DIR` is set to the directory where your LLVM installation's configuration files reside. This is required by CMake to locate LLVM properly.

This project uses CMake as its build system. To build the compiler:

1. **Create a Build Directory**  
   In the root directory of the project, create and change into a new build folder:
   ```bash
   mkdir build
   cd build
   ```

2. **Run CMake**  
   Configure the project using CMake:
   ```bash
   cmake ..
   ```
   (or `cmake .. -DLLVM_DIR=/path/to/lib/cmake/llvm`)

3. **Build the Project**  
   Compile the source files:
   ```bash
   make
   ```

This produces the compiler `Pietric` in the build directory (a Release build unless `CMAKE_BUILD_TYPE` says otherwise).

## Using the Compiler

Pietric takes a Piet program, either an image (PNG, GIF or BMP) or a text file of whitespace-separated hex colour codes, and compiles it into an executable:
```bash
./Pietric path/to/program.png -o program
./program
```
Behind the scenes Pietric runs LLVM's `opt` and `llc` (those of the LLVM it was built with, or else the ones on the `PATH`) to optimize the program and generate machine code for the host, and links it with the C compiler `cc`; the program needs only the C library.

Options:

| Option | Effect |
|---|---|
| `-o <file>` | Output file. Default: `a.out` (or `<program>.ll` / `<program>.o` with `--emit-llvm` / `-c`) |
| `-O0` … `-O3` | Optimization level (default `-O2`). `-O0` compiles fastest, which helps with very large programs |
| `--emit-llvm` | Write LLVM IR, after optimization at the chosen level, instead of an executable |
| `-c` | Write an object file instead of an executable |
| `--cc=<program>` | C compiler used for linking (default `cc`) |
| `-march=<cpu>` | Generate code for the given CPU, e.g. `-march=native` for this machine's or `-march=x86-64-v3`. Faster where loops vectorize, but the executable may not run on older CPUs. Default: any CPU of the host's architecture |
| `-v` | Report the codel size detected in an image |

The IR from `--emit-llvm` can also be built with LLVM's tools, for example:
```bash
./Pietric -O0 --emit-llvm program.png -o program.ll
opt -O3 program.ll -S -o optimized.ll
llc optimized.ll -relocation-model=pic -filetype=obj -o program.o
cc program.o -o program
```

## Optimization

The generated code is laid out for the optimizer. Every state of the program is emitted once for each stack depth it can be reached with (up to 16), so that within each copy the depth is a constant and every stack value is an SSA value. The optimizer can then follow values through the whole program and fold everything that does not depend on input: `tests/cases/pietric.png`, for instance, becomes seven calls that print `pietric`. Deeper stacks keep their top values the same way: the code is emitted once for each number of values (up to 8) in a window at the top, and only the values below it are in a heap buffer, whose size is kept at run time and which grows as needed. Commands work on the window; values move between the window and the heap only when the window fills up, when a command needs more values than the window holds, and for rolls deeper than the window. In very large programs, whose copies would make the code too large, the code beyond a budget of copies keeps every value on the heap; it is split into functions of a few hundred nodes each, which enter one another by tail calls, so that the optimizer and code generator never see one huge function.

## Semantics

Pietric follows the [Piet specification](https://www.dangermouse.net/esoteric/piet.html). Where the specification leaves a choice, Pietric behaves as follows:

- Values are 32-bit signed integers; arithmetic wraps around on overflow.
- The stack grows as needed. If memory runs out, the program reports it on stderr and exits with status 1.
- A command that needs more values than the stack holds is ignored, as the specification prescribes for operations that cannot be performed.
- `divide` truncates towards zero. `mod` gives a result with the sign of the divisor (the top value). Division or modulo by zero is ignored and leaves both operands on the stack, as the specification recommends.
- `roll` with a negative depth, or a depth greater than the number of remaining values, is ignored and leaves its two arguments on the stack.
- `pointer` turns the DP clockwise by the popped value modulo 4 (anticlockwise for negative values); `switch` toggles the CC the absolute value of the popped value times.
- `in(number)` skips whitespace and reads an optionally signed decimal integer; `in(char)` reads one byte. If nothing can be read (end of input, or no number at the current position), nothing is pushed. `out(char)` writes the low byte of the value.
- Output is buffered in the program and written out when 64 KiB have accumulated, before the program reads input, and when it ends; if stdout is a terminal, also after each newline. A program that is killed loses the output it has not written out yet.
- Sliding through white codels follows the 2008 clarification of the specification: the program terminates when it would retrace its route within a white region.
- The program starts at the top-left codel; if it is black or white, the compiled program does nothing.
- All colors other than the 20 Piet colors are treated as one extra color: its codels form blocks like any other color, but moving into or out of them never executes a command.

## Benchmarks

The table under [Performance](#performance) comes from `bench/run.py`. It runs each program on every implementation with the same input, checks the output, and reports the wall-clock time, start-up included. The time is the median of 3 runs for runs under 20 s, and a single run otherwise. Runs are stopped after 600 s.

The programs:

- **Brainfuck interpreter**: [piet_bfi.gif](https://www.dangermouse.net/esoteric/piet/piet_bfi.gif) from the Piet samples page, converted to PNG, since not every implementation reads GIF. It runs Daniel B Cristofani's [squares.b](https://brainfuck.org/squares.b), which prints the squares up to 10,000, and four nested loops of 64, 10, 10 and 10 iterations. The script downloads both files.
- **The other four programs** are generated by `bench/programs.py`. Each reads N and runs one loop: summing c mod 7, counting primes by trial division, reading one of 50 values with rolls 3 to 52 deep, and printing N numbers.

The implementations, as measured (Ubuntu 22.04 on WSL2, Intel Core i5-12400):

| Implementation | Version | Built and run as |
|---|---|---|
| Pietric | this repository | Release build, `-O2` (the default) |
| [PietCC](https://github.com/pwang00/pietcc) | 26e8bec | `cargo build --release` with LLVM 20 (its `llvm20-1` feature instead of the default LLVM 23); `--o2` |
| [npiet](https://www.bertnase.de/npiet/) | 1.3f | `./configure && make` (`-O2`) |
| [rpiet](https://github.com/tessi/rpiet) | 0.3.0 (e6a10ba) | `cargo build --release`; `-c 1` |
| [piet_programming_language](https://github.com/your-diary/piet_programming_language) | 3526991 | `cargo build --release`; `-c 1` |

Compile times of the two compilers:

| Program | Pietric | PietCC |
|---|---|---|
| Brainfuck interpreter | 4.9 s | 8.3 s without optimization (`--o2`: did not finish in 600 s) |
| Sum of c mod 7 | 0.09 s | 0.27 s |
| Primes | 0.16 s | 3.3 s |
| Values deep in the stack | 0.30 s | 11.1 s |
| Printing numbers | 0.10 s | 0.19 s |

To run the benchmarks (Python 3 with Pillow; PietCC also needs `llc` and `clang` on the `PATH`), give the implementations you have:
```bash
bench/run.py --pietric build/Pietric --pietcc path/to/pietcc --npiet path/to/npiet \
    --rpiet path/to/rpiet --piet-rs path/to/piet_programming_language
```

## Testing

The end-to-end tests compile each program in `tests/cases/` with Pietric at `-O0` and at `-O2`, run it and compare its output with the expected output. From the build directory, run:
```bash
ctest --output-on-failure
```
or call the script directly to see each failure (an optional last argument selects tests whose name contains it):
```bash
../tests/run_tests.sh ./Pietric cc ../tests/cases
```

A test case `<name>` consists of the program (`<name>.txt` or an image), the expected stdout (`<name>.out`) and optionally stdin (`<name>.in`). The text-format cases are generated by `tests/gen_cases.py`; to add one, add an entry to `CASES` with its expected output and run `python3 tests/gen_cases.py`.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

## Acknowledgements

- **Piet Language:** Invented by David Morgan-Mar.
- **LLVM:** For providing a robust compiler framework.
- **stb_image:** For easy image loading.
