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
    
    # Check result (modulo 256 for exit codes)
    local expected_mod=$((expected % 256))
    if [ $result -eq $expected_mod ]; then
        echo -e "${GREEN}✓ Passed (returned $result)${NC}"
        return 0
    else
        echo -e "${RED}✗ Failed (expected $expected_mod, got $result)${NC}"
        return 1
    fi
}

test_compile_fail() {
    local source=$1
    local name=$(basename $source .om)
    
    echo -n "Testing $name (compile fail)... "
    
    ./build/omsc $source -o ${name}_fail > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo -e "${GREEN}✓ Failed as expected${NC}"
        return 0
    fi
    
    echo -e "${RED}✗ Unexpected compile success${NC}"
    rm -f ${name}_fail ${name}_fail.o
    return 1
}

test_cli_output() {
    local name=$1
    local expected=$2
    local expected_exit=$3
    shift 3
    
    if [ -z "$expected_exit" ]; then
        expected_exit=0
    fi
    
    echo -n "Testing $name... "
    
    local output
    output=$("$@" 2>&1)
    local status=$?
    
    if [ $status -ne $expected_exit ]; then
        echo -e "${RED}✗ Failed (expected exit $expected_exit, got $status)${NC}"
        echo "$output"
        return 1
    fi
    
    if [ -n "$expected" ] && ! echo "$output" | grep -q "$expected"; then
        echo -e "${RED}✗ Failed (missing expected output)${NC}"
        echo "$output"
        return 1
    fi
    
    echo -e "${GREEN}✓ Passed${NC}"
    return 0
}

# Run tests
echo "Running CLI tests:"
echo "--------------------------------------------"
test_cli_output "help" "Usage:" 0 ./build/omsc --help
test_cli_output "version" "OmScript Compiler v1.0" 0 ./build/omsc version
test_cli_output "lex" "FN" 0 ./build/omsc lex examples/test.om
test_cli_output "lex-flag" "FN" 0 ./build/omsc --lex examples/test.om
test_cli_output "lex-compound-ops" "PLUS_ASSIGN" 0 ./build/omsc lex examples/compound_assign.om
test_cli_output "lex-do-while" "DO" 0 ./build/omsc lex examples/do_while.om
test_cli_output "lex-ternary" "QUESTION" 0 ./build/omsc lex examples/ternary.om
test_cli_output "lex-bitwise" "AMPERSAND" 0 ./build/omsc lex examples/bitwise.om
test_cli_output "tokens-flag" "FN" 0 ./build/omsc --tokens examples/test.om
test_cli_output "parse" "Parsed program" 0 ./build/omsc parse examples/test.om
test_cli_output "parse-flag" "Parsed program" 0 ./build/omsc --parse examples/test.om
test_cli_output "ast-flag" "Parsed program" 0 ./build/omsc --ast examples/test.om
test_cli_output "emit-ir" "define i64 @main" 0 ./build/omsc --emit-ir examples/exit_zero.om
test_cli_output "emit-ir-alias" "define i64 @main" 0 ./build/omsc --ir examples/exit_zero.om
test_cli_output "emit-ir-output-flag" "" 0 ./build/omsc emit-ir examples/exit_zero.om --output emit_ir_flag.ll
if [ ! -f emit_ir_flag.ll ] || ! grep -q "define i64 @main" emit_ir_flag.ll; then
    echo -e "${RED}✗ Failed (emit-ir output flag did not write file)${NC}"
    rm -f emit_ir_flag.ll
    exit 1
fi
rm -f emit_ir_flag.ll
test_cli_output "output-empty" "Error: -o/--output requires a valid output file name" 1 ./build/omsc run examples/exit_zero.om -o ""
test_cli_output "build-flag" "Compilation successful!" 0 ./build/omsc --build examples/exit_zero.om -o build_flag_test
if [ ! -f build_flag_test ] || [ ! -f build_flag_test.o ]; then
    echo -e "${RED}✗ Failed (build flag did not create outputs)${NC}"
    rm -f build_flag_test build_flag_test.o
    exit 1
fi
rm -f build_flag_test build_flag_test.o
test_cli_output "run-success" "Compilation successful!" 0 ./build/omsc run examples/exit_zero.om
test_cli_output "run-flag" "Compilation successful!" 0 ./build/omsc --run examples/exit_zero.om
test_cli_output "run" "Program exited with code 120" 120 ./build/omsc run examples/factorial.om
test_cli_output "print-output" "42" 0 ./build/omsc run examples/print_test.om
test_cli_output "float-print" "3.5" 5 ./build/omsc run examples/float_test.om
test_cli_output "string-var-print" "hello from variable" 0 ./build/omsc run examples/string_var_test.om
if [ -f a.out ] || [ -f a.out.o ]; then
    echo -e "${RED}✗ Failed (temporary output files not cleaned)${NC}"
    rm -f a.out a.out.o
    exit 1
fi
test_cli_output "run-keep-temps-long" "Compilation successful!" 0 ./build/omsc run --keep-temps examples/exit_zero.om
test_cli_output "run-keep-temps-short" "Compilation successful!" 0 ./build/omsc run -k examples/exit_zero.om
if [ ! -f a.out ] || [ ! -f a.out.o ]; then
    echo -e "${RED}✗ Failed (expected temporary outputs to remain)${NC}"
    rm -f a.out a.out.o
    exit 1
fi
test_cli_output "clean" "Cleaned outputs" 0 ./build/omsc -C
if [ -f a.out ] || [ -f a.out.o ]; then
    echo -e "${RED}✗ Failed (clean did not remove outputs)${NC}"
    rm -f a.out a.out.o
    exit 1
fi
echo ""

echo "Running test programs:"
echo "--------------------------------------------"
test_program "examples/factorial.om" 120
test_program "examples/fibonacci.om" 55
test_program "examples/arithmetic.om" 240
test_program "examples/test.om" 84
test_program "examples/optimized_loops.om" 5040
test_program "examples/descending_range.om" 15
test_program "examples/advanced.om" 16
test_program "examples/break_continue.om" 10
test_program "examples/scoping.om" 5
test_program "examples/optmax.om" 10
test_program "examples/postfix.om" 4
test_program "examples/short_circuit.om" 1
test_program "examples/div_zero.om" 1
test_program "examples/mod_zero.om" 1
test_program "examples/refcount_test.om" 97
test_program "examples/compound_assign.om" 76
test_program "examples/block_comments.om" 30
test_program "examples/do_while.om" 16
test_program "examples/print_test.om" 0
test_program "examples/ternary.om" 34
test_program "examples/bitwise.om" 52
test_program "examples/prefix_ops.om" 50
test_program "examples/abs_test.om" 26
test_program "examples/optimization_stress_test.om" 432
test_program "examples/string_test.om" 0
test_program "examples/array_test.om" 245
test_program "examples/stdlib_test.om" 66
test_program "examples/stdlib2_test.om" 255
test_program "examples/float_test.om" 5
test_program "examples/string_var_test.om" 0
test_program "examples/print_return_test.om" 0
test_program "examples/optmax_div_zero.om" 1
test_program "examples/forward_ref_test.om" 24
test_program "examples/stdlib_float_test.om" 29
test_compile_fail "examples/const_fail.om"
test_compile_fail "examples/break_outside_loop.om"
test_compile_fail "examples/continue_outside_loop.om"
test_compile_fail "examples/undefined_var.om"
test_compile_fail "examples/int_overflow.om"
test_cli_output "error-line-info" "line" 1 ./build/omsc examples/undefined_var.om -o /tmp/test_err
test_cli_output "int-overflow-msg" "Integer literal out of range" 1 ./build/omsc examples/int_overflow.om -o /tmp/test_overflow

echo ""
echo "============================================"
echo "Optimization Tests"
echo "============================================"
echo ""

# Test optimization levels
echo -n "Testing with O3 optimization... "
./build/omsc examples/benchmark.om -o benchmark_o3 > /dev/null 2>&1
if [ $? -eq 0 ]; then
    ./benchmark_o3 > /dev/null 2>&1
    rm -f benchmark_o3 benchmark_o3.o
    echo -e "${GREEN}✓ O3 compilation successful${NC}"
else
    echo -e "${RED}✗ O3 compilation failed${NC}"
fi

echo ""
echo "============================================"
echo "Test suite complete!"
echo "============================================"
