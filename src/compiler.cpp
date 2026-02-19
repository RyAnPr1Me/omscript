#include "compiler.h"
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Program.h>

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
    if (verbose_) {
        std::cout << "Compiling " << sourceFile << "..." << std::endl;
    }
    
    // Read source code
    std::string source = readFile(sourceFile);
    
    // Lexical analysis
    if (verbose_) {
        std::cout << "  Lexing..." << std::endl;
    }
    Lexer lexer(source);
    std::vector<Token> tokens;
    try {
        tokens = lexer.tokenize();
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(sourceFile + ": " + e.what());
    }
    
    // Syntax analysis
    if (verbose_) {
        std::cout << "  Parsing..." << std::endl;
    }
    Parser parser(tokens);
    std::unique_ptr<Program> program;
    try {
        program = parser.parse();
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(sourceFile + ": " + e.what());
    }
    
    // Code generation
    if (verbose_) {
        std::cout << "  Generating code..." << std::endl;
    }
    CodeGenerator codegen(optLevel_);
    try {
        codegen.generate(program.get());
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(sourceFile + ": " + e.what());
    }
    
    // Print LLVM IR only in verbose mode
    if (verbose_) {
        std::cout << "  LLVM IR:" << std::endl;
        codegen.getModule()->print(llvm::outs(), nullptr);
    }
    
    // Write object file
    std::string objFile = outputFile + ".o";
    if (verbose_) {
        std::cout << "  Writing object file to " << objFile << "..." << std::endl;
    }
    bool objectFileCreated = false;
    auto cleanupObject = [&]() {
        if (objectFileCreated) {
            std::error_code ec;
            std::filesystem::remove(objFile, ec);
            if (ec) {
                std::cerr << "Warning: failed to clean up temporary object file '" << objFile
                          << "': " << ec.message() << "\n";
            }
        }
    };
    
    try {
        codegen.writeObjectFile(objFile);
        objectFileCreated = true;
        
        // Link to create executable
        if (verbose_) {
            std::cout << "  Linking..." << std::endl;
        }
        // Try gcc first, then cc (POSIX standard), then clang for portability.
        std::string linkerProgram;
        for (const char* candidate : {"gcc", "cc", "clang"}) {
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
        llvm::SmallVector<llvm::StringRef, 8> argRefs;
        argRefs.push_back(linkerProgram);
        for (const auto& arg : linkArgs) {
            argRefs.push_back(arg);
        }
        int result = llvm::sys::ExecuteAndWait(linkerProgram, argRefs);
        
        if (result != 0) {
            cleanupObject();
            throw std::runtime_error("Linking failed with exit code " + std::to_string(result));
        }
        
        // Clean up temporary object file after successful link.
        cleanupObject();
    } catch (...) {
        cleanupObject();
        throw;
    }
    
    std::cerr << "Compilation successful! Output: " << outputFile << std::endl;
}

} // namespace omscript
