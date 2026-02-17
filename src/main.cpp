#include "compiler.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include <iostream>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>

namespace {

constexpr const char* kCompilerVersion = "OmScript Compiler v0.3.1";

void printUsage(const char* progName) {
    std::cout << kCompilerVersion << "\n";
    std::cout << "Usage:\n";
    std::cout << "  " << progName << " <source.om> [-o output]\n";
    std::cout << "  " << progName << " compile <source.om> [-o output]\n";
    std::cout << "  " << progName << " run <source.om> [-o output] [-- args...]\n";
    std::cout << "  " << progName << " lex <source.om>\n";
    std::cout << "  " << progName << " parse <source.om>\n";
    std::cout << "  " << progName << " emit-ir <source.om> [-o output.ll]\n";
    std::cout << "  " << progName << " clean [-o output]\n";
    std::cout << "  " << progName << " version\n";
    std::cout << "  " << progName << " help\n";
    std::cout << "\nOptions:\n";
    std::cout << "  -o, --output <file>  Output file name (default: a.out, stdout for emit-ir)\n";
    std::cout << "  -b, -c, --build, --compile  Compile a source file (default)\n";
    std::cout << "  -r, --run            Compile and run a source file\n";
    std::cout << "  -l, --lex, --tokens  Print lexer tokens\n";
    std::cout << "  -a, -p, --ast, --parse      Parse and summarize the AST\n";
    std::cout << "  -e, -i, --emit-ir, --ir     Emit LLVM IR\n";
    std::cout << "  -C, --clean          Remove outputs\n";
    std::cout << "  -h, --help           Show this help message\n";
    std::cout << "  -k, --keep-temps     Keep temporary outputs when running\n";
    std::cout << "  -v, --version        Show compiler version\n";
    std::cout << "  -V, --verbose        Show detailed compilation output (IR, progress)\n";
    std::cout << "  -O0                  No optimization\n";
    std::cout << "  -O1                  Basic optimization\n";
    std::cout << "  -O2                  Moderate optimization (default)\n";
    std::cout << "  -O3                  Aggressive optimization\n";
}

std::string readSourceFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open file: " + filename);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    file.close();
    return buffer.str();
}

const char* tokenTypeToString(omscript::TokenType type) {
    switch (type) {
        case omscript::TokenType::INTEGER: return "INTEGER";
        case omscript::TokenType::FLOAT: return "FLOAT";
        case omscript::TokenType::STRING: return "STRING";
        case omscript::TokenType::IDENTIFIER: return "IDENTIFIER";
        case omscript::TokenType::FN: return "FN";
        case omscript::TokenType::RETURN: return "RETURN";
        case omscript::TokenType::IF: return "IF";
        case omscript::TokenType::ELSE: return "ELSE";
        case omscript::TokenType::WHILE: return "WHILE";
        case omscript::TokenType::DO: return "DO";
        case omscript::TokenType::FOR: return "FOR";
        case omscript::TokenType::VAR: return "VAR";
        case omscript::TokenType::CONST: return "CONST";
        case omscript::TokenType::BREAK: return "BREAK";
        case omscript::TokenType::CONTINUE: return "CONTINUE";
        case omscript::TokenType::IN: return "IN";
        case omscript::TokenType::TRUE: return "TRUE";
        case omscript::TokenType::FALSE: return "FALSE";
        case omscript::TokenType::NULL_LITERAL: return "NULL_LITERAL";
        case omscript::TokenType::OPTMAX_START: return "OPTMAX_START";
        case omscript::TokenType::OPTMAX_END: return "OPTMAX_END";
        case omscript::TokenType::SWITCH: return "SWITCH";
        case omscript::TokenType::CASE: return "CASE";
        case omscript::TokenType::DEFAULT: return "DEFAULT";
        case omscript::TokenType::PLUS: return "PLUS";
        case omscript::TokenType::MINUS: return "MINUS";
        case omscript::TokenType::STAR: return "STAR";
        case omscript::TokenType::SLASH: return "SLASH";
        case omscript::TokenType::PERCENT: return "PERCENT";
        case omscript::TokenType::ASSIGN: return "ASSIGN";
        case omscript::TokenType::EQ: return "EQ";
        case omscript::TokenType::NE: return "NE";
        case omscript::TokenType::LT: return "LT";
        case omscript::TokenType::LE: return "LE";
        case omscript::TokenType::GT: return "GT";
        case omscript::TokenType::GE: return "GE";
        case omscript::TokenType::AND: return "AND";
        case omscript::TokenType::OR: return "OR";
        case omscript::TokenType::NOT: return "NOT";
        case omscript::TokenType::PLUSPLUS: return "PLUSPLUS";
        case omscript::TokenType::MINUSMINUS: return "MINUSMINUS";
        case omscript::TokenType::PLUS_ASSIGN: return "PLUS_ASSIGN";
        case omscript::TokenType::MINUS_ASSIGN: return "MINUS_ASSIGN";
        case omscript::TokenType::STAR_ASSIGN: return "STAR_ASSIGN";
        case omscript::TokenType::SLASH_ASSIGN: return "SLASH_ASSIGN";
        case omscript::TokenType::PERCENT_ASSIGN: return "PERCENT_ASSIGN";
        case omscript::TokenType::AMPERSAND_ASSIGN: return "AMPERSAND_ASSIGN";
        case omscript::TokenType::PIPE_ASSIGN: return "PIPE_ASSIGN";
        case omscript::TokenType::CARET_ASSIGN: return "CARET_ASSIGN";
        case omscript::TokenType::LSHIFT_ASSIGN: return "LSHIFT_ASSIGN";
        case omscript::TokenType::RSHIFT_ASSIGN: return "RSHIFT_ASSIGN";
        case omscript::TokenType::QUESTION: return "QUESTION";
        case omscript::TokenType::AMPERSAND: return "AMPERSAND";
        case omscript::TokenType::PIPE: return "PIPE";
        case omscript::TokenType::CARET: return "CARET";
        case omscript::TokenType::TILDE: return "TILDE";
        case omscript::TokenType::LSHIFT: return "LSHIFT";
        case omscript::TokenType::RSHIFT: return "RSHIFT";
        case omscript::TokenType::RANGE: return "RANGE";
        case omscript::TokenType::LPAREN: return "LPAREN";
        case omscript::TokenType::RPAREN: return "RPAREN";
        case omscript::TokenType::LBRACE: return "LBRACE";
        case omscript::TokenType::RBRACE: return "RBRACE";
        case omscript::TokenType::LBRACKET: return "LBRACKET";
        case omscript::TokenType::RBRACKET: return "RBRACKET";
        case omscript::TokenType::SEMICOLON: return "SEMICOLON";
        case omscript::TokenType::COMMA: return "COMMA";
        case omscript::TokenType::COLON: return "COLON";
        case omscript::TokenType::DOT: return "DOT";
        case omscript::TokenType::END_OF_FILE: return "END_OF_FILE";
        case omscript::TokenType::INVALID: return "INVALID";
    }
    return "UNKNOWN";
}

void printTokens(const std::vector<omscript::Token>& tokens) {
    for (const auto& token : tokens) {
        std::cout << token.line << ":" << token.column << " "
                  << tokenTypeToString(token.type);
        if (!token.lexeme.empty()) {
            std::cout << " '" << token.lexeme << "'";
        }
        std::cout << "\n";
    }
}

void printProgramSummary(const omscript::Program* program) {
    std::cout << "Parsed program with " << program->functions.size()
              << " function(s).\n";
    if (program->functions.empty()) {
        return;
    }
    std::cout << "Functions:\n";
    for (const auto& fn : program->functions) {
        std::cout << "  " << fn->name << "(";
        for (size_t i = 0; i < fn->parameters.size(); ++i) {
            const auto& param = fn->parameters[i];
            std::cout << param.name;
            if (!param.typeName.empty()) {
                std::cout << ": " << param.typeName;
            }
            if (i + 1 < fn->parameters.size()) {
                std::cout << ", ";
            }
        }
        std::cout << ")";
        if (fn->isOptMax) {
            std::cout << " [OPTMAX]";
        }
        std::cout << "\n";
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    enum class Command {
        Compile,
        Run,
        Lex,
        Parse,
        EmitIR,
        Clean,
        Help,
        Version
    };

    int argIndex = 1;
    std::string firstArg = argv[argIndex];
    Command command = Command::Compile;
    bool commandMatched = false;
    if (firstArg == "help" || firstArg == "-h" || firstArg == "--help") {
        command = Command::Help;
        commandMatched = true;
    } else if (firstArg == "version" || firstArg == "-v" || firstArg == "--version") {
        command = Command::Version;
        commandMatched = true;
    } else if (firstArg == "compile" || firstArg == "build" || firstArg == "-c" ||
               firstArg == "-b" || firstArg == "--compile" || firstArg == "--build") {
        command = Command::Compile;
        argIndex++;
        commandMatched = true;
    } else if (firstArg == "run" || firstArg == "-r" || firstArg == "--run") {
        command = Command::Run;
        argIndex++;
        commandMatched = true;
    } else if (firstArg == "lex" || firstArg == "tokens" || firstArg == "-l" ||
               firstArg == "--lex" || firstArg == "--tokens") {
        command = Command::Lex;
        argIndex++;
        commandMatched = true;
    } else if (firstArg == "parse" || firstArg == "-p" || firstArg == "-a" ||
               firstArg == "--parse" || firstArg == "--ast") {
        command = Command::Parse;
        argIndex++;
        commandMatched = true;
    } else if (firstArg == "emit-ir" || firstArg == "-e" || firstArg == "-i" ||
               firstArg == "--emit-ir" || firstArg == "--ir") {
        command = Command::EmitIR;
        argIndex++;
        commandMatched = true;
    } else if (firstArg == "clean" || firstArg == "-C" || firstArg == "--clean") {
        command = Command::Clean;
        argIndex++;
        commandMatched = true;
    }

    if (!commandMatched && !firstArg.empty() && firstArg[0] != '-') {
        bool hasOmExtension = firstArg.size() >= 3 &&
                              firstArg.substr(firstArg.size() - 3) == ".om";
        if (!hasOmExtension && !std::filesystem::exists(firstArg)) {
            std::cerr << "Error: unknown command '" << firstArg << "'\n";
            printUsage(argv[0]);
            return 1;
        }
    }


    if (command == Command::Help) {
        printUsage(argv[0]);
        return 0;
    }
    if (command == Command::Version) {
        std::cout << kCompilerVersion << "\n";
        return 0;
    }

    std::string sourceFile;
    std::string outputFile = command == Command::EmitIR ? "" : "a.out";
    bool outputSpecified = false;
    bool supportsOutputOption = command == Command::Compile || command == Command::Run ||
                                command == Command::EmitIR || command == Command::Clean;
    bool parsingRunArgs = false;
    bool keepTemps = false;
    bool verbose = false;
    omscript::OptimizationLevel optLevel = omscript::OptimizationLevel::O2;
    std::vector<std::string> runArgs;
    
    // Parse command line arguments
    for (int i = argIndex; i < argc; i++) {
        std::string arg = argv[i];
        if (command == Command::Run && arg == "--") {
            parsingRunArgs = true;
            continue;
        }
        if (!parsingRunArgs && (arg == "-h" || arg == "--help")) {
            printUsage(argv[0]);
            return 0;
        }
        if (!parsingRunArgs && (arg == "-v" || arg == "--version")) {
            std::cout << kCompilerVersion << "\n";
            return 0;
        }
        if (!parsingRunArgs && (arg == "-k" || arg == "--keep-temps")) {
            if (command != Command::Run) {
                std::cerr << "Error: -k/--keep-temps is only supported for run commands\n";
                return 1;
            }
            keepTemps = true;
            continue;
        }
        if (!parsingRunArgs && (arg == "-V" || arg == "--verbose")) {
            verbose = true;
            continue;
        }
        if (!parsingRunArgs && arg.size() == 3 && arg[0] == '-' && arg[1] == 'O' &&
            arg[2] >= '0' && arg[2] <= '3') {
            static constexpr omscript::OptimizationLevel levels[] = {
                omscript::OptimizationLevel::O0, omscript::OptimizationLevel::O1,
                omscript::OptimizationLevel::O2, omscript::OptimizationLevel::O3
            };
            optLevel = levels[arg[2] - '0'];
            continue;
        }
        if (!parsingRunArgs && (arg == "-o" || arg == "--output")) {
            if (!supportsOutputOption) {
                std::cerr << "Error: -o/--output is only supported for compile/run/emit-ir commands\n";
                return 1;
            }
            if (outputSpecified) {
                std::cerr << "Error: output file specified multiple times\n";
                return 1;
            }
            if (i + 1 < argc) {
                const char* nextArg = argv[i + 1];
                if (nextArg[0] == '\0' || nextArg[0] == '-') {
                    std::cerr << "Error: -o/--output requires a valid output file name\n";
                    return 1;
                }
                outputFile = argv[++i];
                outputSpecified = true;
            } else {
                std::cerr << "Error: -o/--output requires an argument\n";
                return 1;
            }
        } else if (!parsingRunArgs && !arg.empty() && arg[0] == '-') {
            std::cerr << "Error: unknown option '" << arg << "'\n";
            return 1;
        } else if (command == Command::Clean) {
            std::cerr << "Error: clean does not accept input files (got '" << arg << "')\n";
            return 1;
        } else if (sourceFile.empty()) {
            sourceFile = arg;
        } else if (command == Command::Run && parsingRunArgs) {
            runArgs.push_back(arg);
        } else {
            std::cerr << "Error: multiple input files specified ('" << sourceFile
                      << "' and '" << arg << "')\n";
            return 1;
        }
    }
    
    if (command == Command::Clean) {
        bool removedAny = false;
        auto removeIfPresent = [&](const std::string& path) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                return;
            }
            if (std::filesystem::remove(path, ec)) {
                removedAny = true;
                return;
            }
            if (ec) {
                std::cerr << "Warning: failed to remove '" << path << "': "
                          << ec.message() << "\n";
            }
        };
        removeIfPresent(outputFile);
        removeIfPresent(outputFile + ".o");
        if (removedAny) {
            std::cout << "Cleaned outputs for " << outputFile << "\n";
        } else {
            std::cout << "Nothing to clean for " << outputFile << "\n";
        }
        return 0;
    }

    if (sourceFile.empty()) {
        std::cerr << "Error: no input file specified\n";
        printUsage(argv[0]);
        return 1;
    }
    
    try {
        if (command == Command::Lex || command == Command::Parse || command == Command::EmitIR) {
            std::string source = readSourceFile(sourceFile);
            omscript::Lexer lexer(source);
            auto tokens = lexer.tokenize();
            if (command == Command::Lex) {
                printTokens(tokens);
                return 0;
            }
            omscript::Parser parser(tokens);
            auto program = parser.parse();
            if (command == Command::Parse) {
                printProgramSummary(program.get());
                return 0;
            }
            omscript::CodeGenerator codegen(optLevel);
            codegen.generate(program.get());
            if (outputFile.empty()) {
                codegen.getModule()->print(llvm::outs(), nullptr);
                llvm::outs().flush();
                return 0;
            }
            std::error_code ec;
            llvm::raw_fd_ostream out(outputFile, ec, llvm::sys::fs::OF_None);
            if (ec) {
                throw std::runtime_error("Could not write IR to file: " + ec.message());
            }
            codegen.getModule()->print(out, nullptr);
            return 0;
        }
        omscript::Compiler compiler;
        compiler.setVerbose(verbose);
        compiler.setOptimizationLevel(optLevel);
        compiler.compile(sourceFile, outputFile);
        if (command == Command::Run) {
            std::filesystem::path runPath = std::filesystem::absolute(outputFile);
            std::string runProgram = runPath.string();
            llvm::SmallVector<llvm::StringRef, 8> argRefs;
            argRefs.push_back(runProgram);
            for (const auto& arg : runArgs) {
                argRefs.push_back(arg);
            }
            int result = llvm::sys::ExecuteAndWait(runProgram, argRefs);
            if (result < 0) {
                std::cerr << "Error: failed to execute program\n";
                return 1;
            }
            if (result != 0) {
                std::cout << "Program exited with code " << result << "\n";
            }
            if (!outputSpecified && !keepTemps) {
                std::error_code ec;
                std::filesystem::remove(outputFile, ec);
                if (ec) {
                    std::cerr << "Warning: failed to remove temporary output file '"
                              << outputFile << "': " << ec.message() << "\n";
                }
                std::filesystem::remove(outputFile + ".o", ec);
                if (ec) {
                    std::cerr << "Warning: failed to remove temporary object file '"
                              << outputFile << ".o': " << ec.message() << "\n";
                }
            }
            return result;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
