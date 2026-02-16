#ifndef COMPILER_H
#define COMPILER_H

#include <string>

namespace omscript {

class Compiler {
public:
    Compiler();
    
    void compile(const std::string& sourceFile, const std::string& outputFile);
    void writeFile(const std::string& filename, const std::string& content);

    /// Control verbosity — when true, prints LLVM IR and progress messages.
    void setVerbose(bool v) { verbose_ = v; }
    bool isVerbose() const { return verbose_; }
    
private:
    std::string readFile(const std::string& filename);
    bool verbose_ = false;
};

} // namespace omscript

#endif // COMPILER_H
