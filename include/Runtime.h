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

    llvm::FunctionCallee malloc;
    llvm::FunctionCallee free;
};

// Defines the runtime functions in `module`.
Runtime defineRuntime(llvm::Module &module);

#endif // RUNTIME_H
