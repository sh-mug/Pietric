# Pietric

Pietric is a C++-centric compiler for the Piet programming language. Piet is an esoteric, stack-based language where programs are painted as abstract images. This compiler uses LLVM IR as its intermediate representation and then generates machine code with a custom runtime library implementing the Piet stack operations.

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
│   └── StackVM.h  
├── src/
│   ├── main.cpp        # Main driver for the compiler
│   ├── Utils.cpp       # Utility functions (e.g., hex string conversion)
│   ├── Parser.cpp      # Parses input files (text files with hex codes or BMP/PNG/GIF images)
│   ├── ImageLoader.cpp # Loads images using the stb_image library
│   ├── Graph.cpp       # Builds the execution graph according to Piet’s DP and CC rules
│   ├── IRBuilder.cpp   # Generates LLVM IR from the execution graph.  
│   │                   # (Includes code for pointer, switch, and I/O commands.)
│   └── StackVM.cpp     # Runtime library (stack operations and I/O) linked into compiled programs
└── tests/
    ├── run_tests.sh    # Compiles, runs and checks every program in cases/
    ├── gen_cases.py    # Generates the text-format test programs in cases/
    └── cases/          # Test programs with their expected output (and input)
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

This produces the compiler `Pietric` and the runtime library `libPietRuntime.a` in the build directory.

## Using the Compiler

The compiler accepts as input a Piet program, either as a text file (with whitespace-separated hex color codes) or as an image file (BMP, PNG, or GIF). The compiler then produces an LLVM IR file (`output.ll`) which you can compile further into an executable.

### Step 1: Generate LLVM IR

Run the compiler by specifying an input file:
```bash
./Pietric path/to/input_file
```
This parses your Piet program and outputs `output.ll` (the LLVM IR file) in the build directory.

### Step 2: Compile the LLVM IR into an Executable

You can use LLVM’s tools and your system compiler to convert the IR into a runnable executable. For example:

1. **Compile to Object File**  
   Use `llc` to compile the LLVM IR to an object file:
   ```bash
   llc output.ll -relocation-model=pic -filetype=obj -o output.o
   ```
   (`-relocation-model=pic` is needed when your linker produces position-independent executables, the default on most Linux distributions.)

2. **Link with the Runtime Library**  
   Link the object file with the runtime library built alongside the compiler, using a C++ compiler (e.g., g++ or clang++):
   ```bash
   g++ output.o libPietRuntime.a -o output
   ```

### Step 3: Run the Executable

Once linked, run your executable:
```bash
./output
```

## Optimizing the LLVM IR

To optimize the generated LLVM IR, you can run LLVM’s optimization passes. For example, using the `opt` tool:
```bash
opt -O3 output.ll -S -o optimized.ll
llc optimized.ll -relocation-model=pic -filetype=obj -o output.o
g++ output.o libPietRuntime.a -o output
```
You can also integrate LLVM’s pass manager into your IR generation pipeline if desired.

## Semantics

Pietric follows the [Piet specification](https://www.dangermouse.net/esoteric/piet.html). Where the specification leaves a choice, Pietric behaves as follows:

- Values are 32-bit signed integers; arithmetic wraps around on overflow.
- A command that needs more values than the stack holds is ignored, as the specification prescribes for operations that cannot be performed.
- `divide` truncates towards zero. `mod` gives a result with the sign of the divisor (the top value). Division or modulo by zero is ignored and leaves both operands on the stack, as the specification recommends.
- `roll` with a negative depth, or a depth greater than the number of remaining values, is ignored and leaves its two arguments on the stack.
- `pointer` turns the DP clockwise by the popped value modulo 4 (anticlockwise for negative values); `switch` toggles the CC the absolute value of the popped value times.
- `in(number)` skips whitespace and reads an optionally signed decimal integer; `in(char)` reads one byte. If nothing can be read (end of input, or no number at the current position), nothing is pushed. `out(char)` writes the low byte of the value.
- Sliding through white codels follows the 2008 clarification of the specification: the program terminates when it would retrace its route within a white region.
- The program starts at the top-left codel; if it is black or white, the compiled program does nothing.
- All colors other than the 20 Piet colors are treated as one extra color: its codels form blocks like any other color, but moving into or out of them never executes a command.

## Testing

The end-to-end tests compile each program in `tests/cases/` with Pietric, link it against the runtime both unoptimized and with `opt -O2`, run it and compare its output with the expected output. From the build directory, run:
```bash
ctest --output-on-failure
```
or call the script directly to see each failure (an optional last argument selects tests whose name contains it):
```bash
../tests/run_tests.sh ./Pietric "$(which llc)" "$(which opt)" c++ ./libPietRuntime.a ../tests/cases
```

A test case `<name>` consists of the program (`<name>.txt` or an image), the expected stdout (`<name>.out`) and optionally stdin (`<name>.in`). The text-format cases are generated by `tests/gen_cases.py`; to add one, add an entry to `CASES` with its expected output and run `python3 tests/gen_cases.py`.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

## Acknowledgements

- **Piet Language:** Invented by David Morgan-Mar.
- **LLVM:** For providing a robust compiler framework.
- **stb_image:** For easy image loading.
