#ifndef IRBUILDER_H
#define IRBUILDER_H

#include "Graph.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LLVMContext.h"

class IRGenerator {
public:
    IRGenerator(llvm::LLVMContext &ctx);
    // Generate an LLVM module from the given graph.
    llvm::Module* generateModule(const Graph &graph);
private:
    llvm::LLVMContext &context;
};

#endif // IRBUILDER_H
