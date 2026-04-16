#include "compiler.h"
#include "codegen.h"
#include "diagnostic.h"
#include "lexer.h"
#include "parser.h"
#include "preprocessor.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Program.h>
#include <sstream>
#include <stdexcept>

namespace omscript {

std::string Compiler::readFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw FileError("Could not open file: " + filename);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void Compiler::writeFile(const std::string& filename, const std::string& content) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        throw FileError("Could not write to file: " + filename);
    }
    file << content;
    if (file.fail()) {
        throw FileError("Error writing to file: " + filename);
    }
}

void Compiler::compile(const std::string& sourceFile, const std::string& outputFile) {
    // Validate input file
    if (sourceFile.empty()) {
        throw ValidationError("Source file name cannot be empty");
    }
    if (sourceFile.size() > kMaxPathLength) {
        throw ValidationError("Source file name too long (max " + std::to_string(kMaxPathLength) + " characters)");
    }
    if (outputFile.empty()) {
        throw ValidationError("Output file name cannot be empty");
    }
    if (outputFile.size() > kMaxPathLength) {
        throw ValidationError("Output file name too long (max " + std::to_string(kMaxPathLength) + " characters)");
    }

    // Check if source file exists before attempting to read
    if (!std::filesystem::exists(sourceFile)) {
        throw FileError("Source file does not exist: " + sourceFile);
    }
    if (std::filesystem::is_directory(sourceFile)) {
        throw FileError("'" + sourceFile + "' is a directory, not a source file");
    }

    // Check file size to prevent memory exhaustion (skip for non-regular files like pipes)
    if (std::filesystem::is_regular_file(sourceFile)) {
        auto fileSize = std::filesystem::file_size(sourceFile);
        if (fileSize > kMaxFileSize) {
            throw FileError("Source file too large (max 100MB): " + sourceFile);
        }
    }

    using Clock = std::chrono::steady_clock;
    const auto compileStart = Clock::now();
    // Helper: milliseconds elapsed since a reference point.
    auto elapsedMs = [&](Clock::time_point ref) -> double {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - ref).count() / 1000.0;
    };

    if (verbose_) {
        std::cout << "Compiling " << sourceFile << "...\n";
    }

    // Read source code
    std::string source = readFile(sourceFile);

    // Preprocessor pass
    const auto ppStart = Clock::now();
    Preprocessor pp(sourceFile);
    source = pp.process(source);
    for (const auto& w : pp.warnings()) {
        std::cerr << w << "\n";
    }

    // Lexical analysis
    if (verbose_) {
        std::cout << "  Preprocessing done [+" << elapsedMs(compileStart) << "ms]: "
                  << pp.macroMap().size() << " macros defined\n";
        std::cout << "  Lexing...\n";
    }
    Lexer lexer(std::move(source));
    std::vector<Token> tokens;
    const auto lexStart = Clock::now();
    try {
        tokens = lexer.tokenize();
    } catch (const DiagnosticError&) {
        throw; // Preserves source location; main.cpp adds filename prefix
    } catch (const std::exception& e) {
        throw FileError(sourceFile + ": " + e.what());
    }
    if (verbose_) {
        std::cout << "  Lex done [+" << elapsedMs(compileStart) << "ms, "
                  << elapsedMs(lexStart) << "ms]: "
                  << tokens.size() << " tokens\n";
    }
    (void)ppStart; // reserved for future per-pp timing

    // Syntax analysis
    if (verbose_) {
        std::cout << "  Parsing...\n";
    }
    const auto parseStart = Clock::now();
    Parser parser(std::move(tokens));
    parser.setBaseDir(std::filesystem::path(sourceFile).parent_path().string());
    std::unique_ptr<Program> program;
    try {
        program = parser.parse();
    } catch (const DiagnosticError&) {
        throw;
    } catch (const std::exception& e) {
        throw FileError(sourceFile + ": " + e.what());
    }
    // Emit parser warnings (annotation conflicts, import preprocessor messages)
    // to stderr so they are visible to the user at compile time.
    for (const auto& w : parser.warnings()) {
        std::cerr << w << "\n";
    }
    if (verbose_) {
        std::cout << "  Parse done [+" << elapsedMs(compileStart) << "ms, "
                  << elapsedMs(parseStart) << "ms]: "
                  << program->functions.size() << " function(s)\n";
    }

    // Code generation
    if (verbose_) {
        std::cout << "  Generating code...\n";
    }
    const auto codegenStart = Clock::now();
    CodeGenerator codegen(optLevel_);
    codegen.setVerbose(verbose_);
    codegen.setMarch(march_);
    codegen.setMtune(mtune_);
    codegen.setPIC(pic_);
    codegen.setFastMath(fastMath_);
    codegen.setOptMax(optMax_);
    codegen.setVectorize(vectorize_);
    codegen.setUnrollLoops(unrollLoops_);
    codegen.setLoopOptimize(loopOptimize_);
    codegen.setParallelize(parallelize_);
    codegen.setLTO(lto_);
    codegen.setDebugMode(debug_);
    codegen.setSourceFilename(sourceFile);
    if (!pgoGenPath_.empty()) {
        codegen.setPGOGen(pgoGenPath_);
    }
    if (!pgoUsePath_.empty()) {
        codegen.setPGOUse(pgoUsePath_);
    }
    try {
        codegen.generate(program.get());
    } catch (const DiagnosticError&) {
        throw;
    } catch (const std::exception& e) {
        throw FileError(sourceFile + ": " + e.what());
    }

    if (verbose_) {
        // Count functions and total instructions in the generated module.
        unsigned irFunctions = 0, irInstructions = 0;
        for (const auto& F : *codegen.getModule()) {
            if (!F.isDeclaration()) {
                ++irFunctions;
                irInstructions += F.getInstructionCount();
            }
        }
        std::cout << "  Codegen done [+" << elapsedMs(compileStart) << "ms, "
                  << elapsedMs(codegenStart) << "ms]: "
                  << irFunctions << " function(s), "
                  << irInstructions << " IR instruction(s)\n";
    }

    // Print LLVM IR only in verbose mode
    if (verbose_) {
        std::cout << "  LLVM IR:\n";
        std::cout.flush(); // synchronize before llvm::outs() writes to the same fd
        codegen.getModule()->print(llvm::outs(), nullptr);
        llvm::outs().flush(); // ensure IR is fully written before std::cout resumes
    }

    // Write object file (or bitcode for FLTO)
    std::string objFile = outputFile + (lto_ ? ".bc" : ".o");
    if (verbose_) {
        if (lto_) {
            std::cout << "  Writing bitcode to " << objFile << " (FLTO enabled)...\n";
        } else {
            std::cout << "  Writing object file to " << objFile << "...\n";
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
            std::cout << "  Linking...\n";
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
            throw LinkError("Failed to locate a C linker (tried gcc, cc, clang)");
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
            // Pass -march to the linker so the LTO backend uses the
            // correct ISA features.  Without this, the LTO pass derives
            // features solely from the target-cpu attribute (e.g. "znver4")
            // which may include AVX-512 even when the host only has AVX2.
            if (!march_.empty()) {
                linkArgs.push_back("-march=" + march_);
            }
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
        if (debug_) {
            linkArgs.push_back("-g");
        }
        // Link against libm for math intrinsic fallbacks (sqrt, floor, etc.)
        linkArgs.push_back("-lm");
        // Link against pthreads for concurrency primitives.
        linkArgs.push_back("-lpthread");
        llvm::SmallVector<llvm::StringRef, 8> argRefs;
        argRefs.push_back(linkerProgram);
        for (const auto& arg : linkArgs) {
            argRefs.push_back(arg);
        }
        const int result = llvm::sys::ExecuteAndWait(linkerProgram, argRefs);

        if (result != 0) {
            cleanupObject();
            if (result < 0) {
                throw LinkError("Linker terminated by signal " + std::to_string(-result));
            }
            throw LinkError("Linking failed with exit code " + std::to_string(result));
        }

        // Clean up temporary object file after successful link.
        cleanupObject();
    } catch (...) {
        cleanupObject();
        throw;
    }

    if (verbose_) {
        std::cout << "  Total compile time: " << elapsedMs(compileStart) << "ms\n";
    }

    if (!quiet_) {
        std::cout << "compiled " << outputFile << "\n";
    }
}

} // namespace omscript
