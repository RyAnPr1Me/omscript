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
    void writeFile(const std::string& filename, const std::string& content);
};

} // namespace omscript

#endif // COMPILER_H
