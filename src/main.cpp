#include "compiler.h"
#include <iostream>
#include <cstring>

void printUsage(const char* progName) {
    std::cout << "OmScript Compiler v1.0\n";
    std::cout << "Usage: " << progName << " <source.om> [-o output]\n";
    std::cout << "\nOptions:\n";
    std::cout << "  -o <file>    Output file name (default: a.out)\n";
    std::cout << "  -h, --help   Show this help message\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string sourceFile;
    std::string outputFile = "a.out";
    bool outputSpecified = false;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (outputSpecified) {
                std::cerr << "Error: output file specified multiple times\n";
                return 1;
            }
            if (i + 1 < argc) {
                if (argv[i + 1][0] == '-') {
                    std::cerr << "Error: -o requires a valid output file name\n";
                    return 1;
                }
                outputFile = argv[++i];
                outputSpecified = true;
            } else {
                std::cerr << "Error: -o requires an argument\n";
                return 1;
            }
        } else if (argv[i][0] == '-') {
            std::cerr << "Error: unknown option '" << argv[i] << "'\n";
            return 1;
        } else if (sourceFile.empty()) {
            sourceFile = argv[i];
        } else {
            std::cerr << "Error: multiple input files specified ('" << sourceFile
                      << "' and '" << argv[i] << "')\n";
            return 1;
        }
    }
    
    if (sourceFile.empty()) {
        std::cerr << "Error: no input file specified\n";
        printUsage(argv[0]);
        return 1;
    }
    
    try {
        omscript::Compiler compiler;
        compiler.compile(sourceFile, outputFile);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
