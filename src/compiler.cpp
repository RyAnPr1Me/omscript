#include "compiler.h"
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
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
    return buffer.str();
}

void Compiler::writeFile(const std::string& filename, const std::string& content) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Could not write to file: " + filename);
    }
    file << content;
}

void Compiler::compile(const std::string& sourceFile, const std::string& outputFile) {
    std::cout << "Compiling " << sourceFile << "..." << std::endl;
    
    // Read source code
    std::string source = readFile(sourceFile);
    
    // Lexical analysis
    std::cout << "  Lexing..." << std::endl;
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    
    // Syntax analysis
    std::cout << "  Parsing..." << std::endl;
    Parser parser(tokens);
    auto program = parser.parse();
    
    // Code generation
    std::cout << "  Generating code..." << std::endl;
    CodeGenerator codegen;
    codegen.generate(program.get());
    
    // Print LLVM IR
    std::cout << "  LLVM IR:" << std::endl;
    codegen.getModule()->print(llvm::outs(), nullptr);
    
    // Write object file
    std::string objFile = outputFile + ".o";
    std::cout << "  Writing object file to " << objFile << "..." << std::endl;
    codegen.writeObjectFile(objFile);
    
    // Link to create executable
    std::cout << "  Linking..." << std::endl;
    auto gccPath = llvm::sys::findProgramByName("gcc");
    if (!gccPath) {
        throw std::runtime_error("Failed to locate gcc for linking");
    }
    std::vector<std::string> linkArgs = {*gccPath, objFile, "-o", outputFile};
    llvm::SmallVector<llvm::StringRef, 8> argRefs;
    for (const auto& arg : linkArgs) {
        argRefs.push_back(arg);
    }
    int result = llvm::sys::ExecuteAndWait(linkArgs.front(), argRefs);
    
    if (result != 0) {
        throw std::runtime_error("Linking failed with exit code " + std::to_string(result));
    }
    
    std::cout << "Compilation successful! Output: " << outputFile << std::endl;
}

} // namespace omscript
