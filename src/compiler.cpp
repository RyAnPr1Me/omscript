#include "compiler.h"
#include "codegen.h"
#include "diagnostic.h"
#include "lexer.h"
#include "parser.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Program.h>
#include <sstream>
#include <stdexcept>

namespace omscript {

Compiler::Compiler() {}

std::string Compiler::readFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open file: " + filename);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    file.close();
    return buffer.str();
}

void Compiler::writeFile(const std::string& filename, const std::string& content) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Could not write to file: " + filename);
    }
    file << content;
    if (file.fail()) {
        throw std::runtime_error("Error writing to file: " + filename);
    }
    file.close();
}

void Compiler::compile(const std::string& sourceFile, const std::string& outputFile) {
    // Validate input file
    if (sourceFile.empty()) {
        throw std::runtime_error("Source file name cannot be empty");
    }
    if (sourceFile.size() > 4096) {
        throw std::runtime_error("Source file name too long (max 4096 characters)");
    }
    if (outputFile.empty()) {
        throw std::runtime_error("Output file name cannot be empty");
    }
    if (outputFile.size() > 4096) {
        throw std::runtime_error("Output file name too long (max 4096 characters)");
    }

    // Check if source file exists before attempting to read
    if (!std::filesystem::exists(sourceFile)) {
        throw std::runtime_error("Source file does not exist: " + sourceFile);
    }

    // Check file size to prevent memory exhaustion
    auto fileSize = std::filesystem::file_size(sourceFile);
    if (fileSize > size_t{100} * 1024 * 1024) { // 100MB limit
        throw std::runtime_error("Source file too large (max 100MB): " + sourceFile);
    }

    if (verbose_) {
        std::cout << "Compiling " << sourceFile << "..." << std::endl;
    }

    // Read source code
    std::string source = readFile(sourceFile);

    // Lexical analysis
    if (verbose_) {
        std::cout << "  Lexing..." << std::endl;
    }
    Lexer lexer(std::move(source));
    std::vector<Token> tokens;
    try {
        tokens = lexer.tokenize();
    } catch (const DiagnosticError&) {
        throw; // Preserves source location; main.cpp adds filename prefix
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(sourceFile + ": " + e.what());
    }

    // Syntax analysis
    if (verbose_) {
        std::cout << "  Parsing..." << std::endl;
    }
    Parser parser(std::move(tokens));
    std::unique_ptr<Program> program;
    try {
        program = parser.parse();
    } catch (const DiagnosticError&) {
        throw;
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(sourceFile + ": " + e.what());
    }

    // Code generation
    if (verbose_) {
        std::cout << "  Generating code..." << std::endl;
    }
    CodeGenerator codegen(optLevel_);
    codegen.setMarch(march_);
    codegen.setMtune(mtune_);
    codegen.setPIC(pic_);
    codegen.setFastMath(fastMath_);
    codegen.setOptMax(optMax_);
    codegen.setVectorize(vectorize_);
    codegen.setUnrollLoops(unrollLoops_);
    codegen.setLoopOptimize(loopOptimize_);
    if (!pgoGenPath_.empty()) {
        codegen.setPGOGen(pgoGenPath_);
    }
    if (!pgoUsePath_.empty()) {
        codegen.setPGOUse(pgoUsePath_);
    }
    try {
        // Compile all functions to LLVM IR.  When JIT is enabled the resulting
        // module is used by the adaptive JIT runtime (Tier-1 native code via
        // MCJIT, with call-count monitoring and O3+PGO hot recompile).
        if (jit_) {
            codegen.generateHybrid(program.get());
        } else {
            codegen.generate(program.get());
        }
    } catch (const DiagnosticError&) {
        throw;
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(sourceFile + ": " + e.what());
    }

    // Print LLVM IR only in verbose mode
    if (verbose_) {
        std::cout << "  LLVM IR:" << std::endl;
        codegen.getModule()->print(llvm::outs(), nullptr);
    }

    // Write object file (or bitcode for FLTO)
    std::string objFile = outputFile + (lto_ ? ".bc" : ".o");
    if (verbose_) {
        if (lto_) {
            std::cout << "  Writing bitcode to " << objFile << " (FLTO enabled)..." << std::endl;
        } else {
            std::cout << "  Writing object file to " << objFile << "..." << std::endl;
        }
    }
    bool objectFileCreated = false;
    auto cleanupObject = [&]() {
        if (objectFileCreated) {
            std::error_code ec;
            std::filesystem::remove(objFile, ec);
            if (ec) {
                std::cerr << "Warning: failed to clean up temporary file '" << objFile << "': " << ec.message() << "\n";
            }
            objectFileCreated = false;
        }
    };

    try {
        if (lto_) {
            codegen.writeBitcodeFile(objFile);
        } else {
            codegen.writeObjectFile(objFile);
        }
        objectFileCreated = true;

        // Link to create executable
        if (verbose_) {
            std::cout << "  Linking..." << std::endl;
        }
        // When LTO is enabled, prefer clang which can link LLVM bitcode files.
        // Otherwise try gcc first, then cc (POSIX standard), then clang.
        std::string linkerProgram;
        auto ltoLinkers = {"clang", "gcc", "cc"};
        auto defaultLinkers = {"gcc", "cc", "clang"};
        for (const char* candidate : (lto_ ? ltoLinkers : defaultLinkers)) {
            auto path = llvm::sys::findProgramByName(candidate);
            if (path) {
                linkerProgram = *path;
                break;
            }
        }
        if (linkerProgram.empty()) {
            throw std::runtime_error("Failed to locate a C linker (tried gcc, cc, clang)");
        }
        std::vector<std::string> linkArgs = {objFile, "-o", outputFile};
        // Pass optimization level to the linker for better code layout,
        // dead code elimination, and link-time optimizations.
        switch (optLevel_) {
        case OptimizationLevel::O1:
            linkArgs.push_back("-O1");
            break;
        case OptimizationLevel::O2:
            linkArgs.push_back("-O2");
            break;
        case OptimizationLevel::O3:
            linkArgs.push_back("-O3");
            break;
        default:
            break;
        }
        if (lto_) {
            linkArgs.push_back("-flto");
        }
        if (staticLink_) {
            linkArgs.push_back("-static");
        }
        if (strip_) {
            linkArgs.push_back("-s");
        }
        if (stackProtector_) {
            linkArgs.push_back("-fstack-protector-strong");
        }
        // Link against libm for math intrinsic fallbacks (sqrt, floor, etc.)
        linkArgs.push_back("-lm");
        llvm::SmallVector<llvm::StringRef, 8> argRefs;
        argRefs.push_back(linkerProgram);
        for (const auto& arg : linkArgs) {
            argRefs.push_back(arg);
        }
        int result = llvm::sys::ExecuteAndWait(linkerProgram, argRefs);

        if (result != 0) {
            cleanupObject();
            if (result < 0) {
                throw std::runtime_error("Linker terminated by signal " + std::to_string(-result));
            }
            throw std::runtime_error("Linking failed with exit code " + std::to_string(result));
        }

        // Clean up temporary object file after successful link.
        cleanupObject();
    } catch (...) {
        cleanupObject();
        throw;
    }

    std::cout << "compiled " << outputFile << std::endl;
}

} // namespace omscript
