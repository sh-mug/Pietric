#include "Backend.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

namespace {

// Looks for an LLVM tool next to the LLVM that Pietric was built with, then on the PATH.
bool findLLVMTool(const std::string &name, std::string &path, std::string &error) {
    SmallString<256> candidate(PIETRIC_LLVM_TOOLS_DIR);
    sys::path::append(candidate, name);
    if (sys::fs::can_execute(candidate)) {
        path = candidate.str().str();
        return true;
    }
    ErrorOr<std::string> found = sys::findProgramByName(name);
    if (!found) {
        error = "cannot find LLVM's " + name + " (looked in " PIETRIC_LLVM_TOOLS_DIR
                " and on the PATH)";
        return false;
    }
    path = *found;
    return true;
}

// Targets the module at the host. opt and llc derive the data layout from the triple.
void targetHost(Module &module) {
#if LLVM_VERSION_MAJOR >= 21
    module.setTargetTriple(Triple(sys::getDefaultTargetTriple()));
#else
    module.setTargetTriple(sys::getDefaultTargetTriple());
#endif
}

bool writeText(Module &module, const std::string &path, std::string &error) {
    std::error_code ec;
    raw_fd_ostream out(path, ec, sys::fs::OF_Text);
    if (ec) {
        error = "cannot write " + path + ": " + ec.message();
        return false;
    }
    module.print(out, nullptr);
    return true;
}

} // namespace

std::unique_ptr<Backend> Backend::create(unsigned optLevel, const std::string &cc,
                                         std::string &error) {
    std::string opt, llc;
    if (!findLLVMTool("opt", opt, error) || !findLLVMTool("llc", llc, error))
        return nullptr;
    ErrorOr<std::string> ccPath = sys::findProgramByName(cc);
    if (!ccPath) {
        error = "cannot find the C compiler '" + cc + "' to link with (see --cc)";
        return nullptr;
    }
    return std::unique_ptr<Backend>(new Backend(optLevel, opt, llc, *ccPath));
}

Backend::Backend(unsigned optLevel, std::string opt, std::string llc, std::string cc)
    : optLevel(optLevel), opt(std::move(opt)), llc(std::move(llc)), cc(std::move(cc)) { }

Backend::~Backend() {
    for (const std::string &path : temporaries)
        sys::fs::remove(path);
}

bool Backend::run(const std::string &program, const std::vector<std::string> &args,
                  std::string &error) {
    std::vector<StringRef> argv = { program };
    argv.insert(argv.end(), args.begin(), args.end());
    std::string message;
    int status = sys::ExecuteAndWait(program, argv, std::nullopt, {}, 0, 0, &message);
    if (status != 0) {
        error = message.empty()
                    ? sys::path::filename(program).str() + " failed (exit status " +
                          std::to_string(status) + ")"
                    : message;
        return false;
    }
    return true;
}

bool Backend::temporary(const char *extension, std::string &path, std::string &error) {
    SmallString<128> name;
    if (std::error_code ec = sys::fs::createTemporaryFile("pietric", extension, name)) {
        error = "cannot create a temporary file: " + ec.message();
        return false;
    }
    path = name.str().str();
    temporaries.push_back(path);
    return true;
}

bool Backend::writeBitcode(Module &module, std::string &path, std::string &error) {
    targetHost(module);
    if (!temporary("bc", path, error))
        return false;
    std::error_code ec;
    raw_fd_ostream out(path, ec, sys::fs::OF_None);
    if (ec) {
        error = "cannot write " + path + ": " + ec.message();
        return false;
    }
    WriteBitcodeToFile(module, out);
    return true;
}

bool Backend::writeIR(Module &module, const std::string &path, std::string &error) {
    if (optLevel == 0) {
        targetHost(module);
        return writeText(module, path, error);
    }
    std::string bitcode;
    return writeBitcode(module, bitcode, error) &&
           run(opt, { "-O" + std::to_string(optLevel), "-S", bitcode, "-o", path }, error);
}

bool Backend::writeObject(Module &module, const std::string &path, std::string &error) {
    std::string bitcode;
    if (!writeBitcode(module, bitcode, error))
        return false;
    if (optLevel > 0) {
        std::string optimized;
        if (!temporary("bc", optimized, error) ||
            !run(opt, { "-O" + std::to_string(optLevel), bitcode, "-o", optimized }, error))
            return false;
        bitcode = optimized;
    }
    // Position-independent code, since C compilers commonly link position-independent
    // executables by default.
    return run(llc, { "-O" + std::to_string(optLevel), "-relocation-model=pic",
                      "-filetype=obj", bitcode, "-o", path }, error);
}

bool Backend::writeExecutable(Module &module, const std::string &path, std::string &error) {
    std::string object;
    return temporary("o", object, error) && writeObject(module, object, error) &&
           run(cc, { object, "-o", path }, error);
}
