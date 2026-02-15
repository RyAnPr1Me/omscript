#ifndef VM_H
#define VM_H

#include "value.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <cstdint>

namespace omscript {

class VM {
public:
    VM();
    
    void execute(const std::vector<uint8_t>& bytecode);
    void setGlobal(const std::string& name, const Value& value);
    Value getGlobal(const std::string& name);
    Value getLastReturn() const { return lastReturn; }
    
private:
    std::vector<Value> stack;
    std::unordered_map<std::string, Value> globals;
    std::vector<Value> locals;
    Value lastReturn;
    
    void push(const Value& value);
    Value pop();

    
    void ensureReadable(const std::vector<uint8_t>& code, size_t ip, size_t count);
    uint8_t readByte(const std::vector<uint8_t>& code, size_t& ip);
    uint16_t readShort(const std::vector<uint8_t>& code, size_t& ip);
    int64_t readInt(const std::vector<uint8_t>& code, size_t& ip);
    double readFloat(const std::vector<uint8_t>& code, size_t& ip);
    std::string readString(const std::vector<uint8_t>& code, size_t& ip);
};

} // namespace omscript

#endif // VM_H
