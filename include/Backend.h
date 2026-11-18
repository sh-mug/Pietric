#ifndef BACKEND_H
#define BACKEND_H

#include "llvm/IR/Module.h"
#include <memory>
#include <string>
#include <vector>

// Turns a generated module into optimized LLVM IR, an object file or an
// executable for the host, by running LLVM's opt and llc and a C compiler.
// (Running the tools keeps Pietric small: linking LLVM's optimizer and code
// generator into it would make it some 50 MB larger.)
class Backend {
public:
    // optLevel is 0 to 3, as in -O0 to -O3; cpu is the CPU to generate code
    // for, as in llc's -mcpu (empty for a generic one, "native" for the host's);
    // cc is the C compiler that links executables. Returns nullptr and sets
    // `error` if a tool is missing.
    static std::unique_ptr<Backend> create(unsigned optLevel, const std::string &cpu,
                                           const std::string &cc, std::string &error);
    ~Backend();  // removes the temporary files

    bool writeIR(llvm::Module &module, const std::string &path, std::string &error);
    bool writeObject(llvm::Module &module, const std::string &path, std::string &error);
    bool writeExecutable(llvm::Module &module, const std::string &path, std::string &error);

private:
    Backend(unsigned optLevel, std::string cpu, std::string opt, std::string llc,
            std::string cc);
    // The options for opt and llc: the optimization level and the CPU; and those for each.
    std::vector<std::string> toolOptions() const;
    std::vector<std::string> optOptions() const;
    std::vector<std::string> llcOptions() const;
    // Targets the module at the host and writes it as bitcode to a temporary file.
    bool writeBitcode(llvm::Module &module, std::string &path, std::string &error);
    // Returns a new temporary file name with the given extension.
    bool temporary(const char *extension, std::string &path, std::string &error);
    bool run(const std::string &program, const std::vector<std::string> &args,
             std::string &error);

    unsigned optLevel;
    std::string cpu, opt, llc, cc;
    std::vector<std::string> temporaries;
};

#endif // BACKEND_H
