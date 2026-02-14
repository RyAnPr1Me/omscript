#ifndef COMPILER_H
#define COMPILER_H

#include <string>

namespace omscript {

class Compiler {
public:
    Compiler();
    
    void compile(const std::string& sourceFile, const std::string& outputFile);
    
private:
    std::string readFile(const std::string& filename);
};

} // namespace omscript

#endif // COMPILER_H
