#include "Parser.h"
#include "Graph.h"
#include "IRBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include <iostream>
#include <fstream>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: pietc <input_file>\n";
        return 1;
    }
    std::string inputFilename = argv[1];

    // 1. Parse the Piet program (text or image).
    Parser parser;
    if (!parser.parseFile(inputFilename)) {
        std::cerr << "Failed to parse the input file.\n";
        return 1;
    }
    auto grid = parser.getGrid();
    if (grid.empty()) {
        std::cerr << "Error: empty input.\n";
        return 1;
    }

    // 2. Build the execution graph.
    Graph graph;
    graph.buildGraph(grid);

    // 3. Generate LLVM IR.
    llvm::LLVMContext context;
    IRGenerator irgen(context);
    llvm::Module *module = irgen.generateModule(graph);

    // 4. Output the LLVM IR to a file.
    std::error_code EC;
    llvm::raw_fd_ostream out("output.ll", EC, llvm::sys::fs::OF_Text);
    if (EC) {
        std::cerr << "Error opening output file: " << EC.message() << "\n";
        return 1;
    }
    module->print(out, nullptr);
    delete module;

    std::cout << "Compilation successful. LLVM IR written to output.ll\n";
    return 0;
}
