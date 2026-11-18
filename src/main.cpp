#include "Backend.h"
#include "Graph.h"
#include "IRBuilder.h"
#include "Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>
#include <string>

using namespace llvm;

static cl::OptionCategory pietricCategory("Pietric options");

static cl::opt<std::string> inputFilename(cl::Positional, cl::Required,
    cl::desc("<program: image (.png, .gif, .bmp) or text file of hex colours>"),
    cl::cat(pietricCategory));

static cl::opt<std::string> outputFilename("o",
    cl::desc("Write the output to <file> (default: a.out, or <program>.ll / <program>.o)"),
    cl::value_desc("file"), cl::cat(pietricCategory));

static cl::opt<unsigned> optLevel("O",
    cl::desc("Optimization level: -O0, -O1, -O2 (default) or -O3"), cl::Prefix, cl::init(2),
    cl::cat(pietricCategory));

static cl::opt<bool> emitLLVM("emit-llvm",
    cl::desc("Write LLVM IR (after optimization at the chosen level) instead of an executable"),
    cl::cat(pietricCategory));

static cl::opt<bool> compileOnly("c",
    cl::desc("Write an object file instead of an executable"), cl::cat(pietricCategory));

static cl::opt<std::string> targetCPU("march",
    cl::desc("Generate code for <cpu>, or for this machine's CPU with -march=native "
             "(default: any CPU of the host's architecture)"),
    cl::value_desc("cpu"), cl::cat(pietricCategory));

static cl::opt<std::string> linker("cc",
    cl::desc("C compiler used to link the executable (default: cc)"), cl::value_desc("program"),
    cl::init("cc"), cl::cat(pietricCategory));

static cl::opt<bool> verbose("v", cl::desc("Report the codel size of an image"),
    cl::cat(pietricCategory));

static int fail(const std::string &message) {
    errs() << "pietric: error: " << message << "\n";
    return 1;
}

int main(int argc, char **argv) {
    cl::HideUnrelatedOptions(pietricCategory);
    cl::ParseCommandLineOptions(argc, argv,
        "Pietric: compiles a Piet program into an executable\n");
    if (optLevel > 3)
        return fail("unknown optimization level -O" + std::to_string(optLevel));
    if (emitLLVM && compileOnly)
        return fail("-c and --emit-llvm cannot be combined");

    // Like a C compiler: a.out by default, or the program's name with the
    // extension replaced, in the current directory.
    std::string output = outputFilename;
    if (output.empty()) {
        std::string stem = sys::path::stem(inputFilename).str();
        output = emitLLVM ? stem + ".ll" : compileOnly ? stem + ".o" : "a.out";
    }

    // 1. Parse the Piet program (text or image).
    Parser parser;
    if (!parser.parseFile(inputFilename))
        return 1;  // the parser has reported why
    const auto &grid = parser.getGrid();
    if (grid.empty())
        return fail("the program " + inputFilename + " is empty");
    if (verbose)
        errs() << "codel size: " << parser.getCodelSize() << "\n";

    // 2. Build the execution graph.
    Graph graph;
    graph.buildGraph(grid);

    // 3. Generate LLVM IR.
    LLVMContext context;
    IRGenerator irgen(context);
    std::unique_ptr<Module> module(irgen.generateModule(graph));
    if (verifyModule(*module, &errs()))
        return fail("internal error: the generated IR is invalid");

    // 4. Optimize, and write the IR, an object file or an executable.
    std::string error;
    std::unique_ptr<Backend> backend = Backend::create(optLevel, targetCPU, linker, error);
    if (!backend)
        return fail(error);
    bool ok = emitLLVM      ? backend->writeIR(*module, output, error)
              : compileOnly ? backend->writeObject(*module, output, error)
                            : backend->writeExecutable(*module, output, error);
    return ok ? 0 : fail(error);
}
