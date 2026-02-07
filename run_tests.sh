#!/bin/bash

echo "============================================"
echo "OmScript Compiler Test Suite"
echo "============================================"
echo ""

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Build the compiler
echo "Building compiler..."
cd "$(dirname "$0")"
mkdir -p build
cd build
cmake .. > /dev/null 2>&1
make -j$(nproc) > /dev/null 2>&1

if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Build failed${NC}"
    exit 1
fi
echo -e "${GREEN}✓ Build successful${NC}"
echo ""

cd ..

# Test each example
test_program() {
    local source=$1
    local expected=$2
    local name=$(basename $source .om)
    
    echo -n "Testing $name... "
    
    # Compile
    ./build/omsc $source -o $name > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo -e "${RED}✗ Compilation failed${NC}"
        return 1
    fi
    
    # Run
    ./$name
    local result=$?
    
    # Clean up
    rm -f $name ${name}.o
    
    # Check result
    if [ $result -eq $expected ]; then
        echo -e "${GREEN}✓ Passed (returned $result)${NC}"
        return 0
    else
        echo -e "${RED}✗ Failed (expected $expected, got $result)${NC}"
        return 1
    fi
}

# Run tests
echo "Running test programs:"
echo "--------------------------------------------"
test_program "examples/factorial.om" 120
test_program "examples/fibonacci.om" 55
test_program "examples/arithmetic.om" 240
test_program "examples/test.om" 84

echo ""
echo "============================================"
echo "Test suite complete!"
echo "============================================"
