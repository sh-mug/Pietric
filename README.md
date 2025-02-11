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
└── src/
    ├── main.cpp        # Main driver for the compiler
    ├── Utils.cpp       # Utility functions (e.g., hex string conversion)
    ├── Parser.cpp      # Parses input files (text files with hex codes or BMP/PNG/GIF images)
    ├── ImageLoader.cpp # Loads images using the stb_image library
    ├── Graph.cpp       # Builds the execution graph according to Piet’s DP and CC rules
    ├── IRBuilder.cpp   # Generates LLVM IR from the execution graph.  
    │                   # (Includes code for pointer, switch, and I/O commands.)
    └── StackVM.cpp     # Implements the runtime “StackVM” library with fast, variable-length stack operations
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

3. **Build the Project**  
   Compile the source files:
   ```bash
   make
   ```

An executable named `Pietric` will be produced in the build directory.

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
   Use `llc` to compile the LLVM IR to an object file (ensuring opaque pointers are enabled):
   ```bash
   llc output.ll -opaque-pointers -filetype=obj -o output.o
   ```

2. **Generate StackVM.o**
    The runtime functions (e.g., in StackVM.cpp) must be compiled into an object file. From the root directory, run:
    ```bash
    g++ -c ../src/StackVM.cpp -I ../include -o StackVM.o
    ```
    (You may also need to compile additional runtime source files if you have split your runtime across multiple files.)

3. **Link with the Runtime Library**  
   Compile the runtime (e.g., `StackVM.cpp`, along with any other needed runtime source files) and link everything together using a C++ compiler (e.g., g++ or clang++):
   ```bash
   # For example, if you already built StackVM.o from StackVM.cpp:
   g++ output.o StackVM.o -o output
   ```
   Make sure to include object files for any additional runtime functions (like `pointerOp`, `switchOp`, `inputNum`, etc.).

### Step 3: Run the Executable

Once linked, run your executable:
```bash
./output
```

## Optimizing the LLVM IR

To optimize the generated LLVM IR, you can run LLVM’s optimization passes. For example, using the `opt` tool:
```bash
opt -O3 output.ll -opaque-pointers -S -o optimized.ll
llc optimized.ll -opaque-pointers -filetype=obj -o output.o
g++ output.o StackVM.o -o output
```
You can also integrate LLVM’s pass manager into your IR generation pipeline if desired.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

## Acknowledgements

- **Piet Language:** Invented by David Morgan-Mar.
- **LLVM:** For providing a robust compiler framework.
- **stb_image:** For easy image loading.
