#ifndef RUNTIME_H
#define RUNTIME_H

#include "llvm/IR/Module.h"

// Support functions for compiled Piet programs. They are defined in the
// generated module itself, with internal linkage, on top of the C library.
struct Runtime {
    // ptr (ptr buffer, i64 capacity): reallocates the stack buffer to hold
    // `capacity` values and returns it. Exits the program if memory runs out.
    llvm::Function *grow;
    // void (): reports that memory ran out and exits with status 1.
    llvm::Function *outOfMemory;
    // void (ptr base, i32 depth, i32 rolls): rolls the `depth` values starting
    // at `base` (the last one being the top of the stack) by `rolls`, as the
    // roll command does. Requires depth >= 0.
    llvm::Function *roll;

    // { i1, i32 } (): reads an integer from stdin, as in(number) does: skips
    // whitespace, then reads an optional sign and at least one digit
    // (out-of-range values wrap around). Returns whether a number was read,
    // and the number; the character that ended the attempt stays unread.
    llvm::Function *readNumber;
    // { i1, i32 } (): reads one byte from stdin. Returns false at end of input.
    llvm::Function *readChar;
    // Output is collected in a buffer of the module and written to stdout
    // with write(2) when the buffer is full, before reading input and when
    // the program ends; when stdout is a terminal, also after each newline.
    //
    // void (): decides how to buffer stdout. Called once at the start.
    llvm::Function *start;
    // void (i32): writes a value to stdout as a decimal number.
    llvm::Function *writeNumber;
    // void (i32): writes the low byte of a value to stdout.
    llvm::Function *writeChar;
    // void (): writes out the buffered output.
    llvm::Function *flush;

    llvm::FunctionCallee malloc;
    llvm::FunctionCallee free;
};

// Defines the runtime functions in `module`.
Runtime defineRuntime(llvm::Module &module);

#endif // RUNTIME_H
