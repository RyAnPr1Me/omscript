#!/bin/bash
set -uo pipefail

echo "============================================"
echo "OmScript Compiler Test Suite"
echo "============================================"
echo ""

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

FAILURES=0
TOTAL=0

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
    
    TOTAL=$((TOTAL + 1))
    echo -n "Testing $name... "
    
    # Compile
    ./build/omsc $source -o $name > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo -e "${RED}✗ Compilation failed${NC}"
        FAILURES=$((FAILURES + 1))
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
        FAILURES=$((FAILURES + 1))
        return 1
    fi
}

test_compile_fail() {
    local source=$1
    local name=$(basename $source .om)
    
    TOTAL=$((TOTAL + 1))
    echo -n "Testing $name (compile fail)... "
    
    ./build/omsc $source -o ${name}_fail > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo -e "${GREEN}✓ Failed as expected${NC}"
        return 0
    fi
    
    echo -e "${RED}✗ Unexpected compile success${NC}"
    rm -f ${name}_fail ${name}_fail.o
    FAILURES=$((FAILURES + 1))
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
    
    TOTAL=$((TOTAL + 1))
    echo -n "Testing $name... "
    
    local output
    output=$("$@" 2>&1)
    local status=$?
    
    if [ $status -ne $expected_exit ]; then
        echo -e "${RED}✗ Failed (expected exit $expected_exit, got $status)${NC}"
        echo "$output"
        FAILURES=$((FAILURES + 1))
        return 1
    fi
    
    if [ -n "$expected" ] && ! echo "$output" | grep -qF -- "$expected"; then
        echo -e "${RED}✗ Failed (missing expected output)${NC}"
        echo "$output"
        FAILURES=$((FAILURES + 1))
        return 1
    fi
    
    echo -e "${GREEN}✓ Passed${NC}"
    return 0
}

# Run tests
echo "Running CLI tests:"
echo "--------------------------------------------"
test_cli_output "help" "Usage:" 0 ./build/omsc --help
test_cli_output "help-command" "Usage:" 0 ./build/omsc help
test_cli_output "parse-leading-verbose" "Parsed program" 0 ./build/omsc -V parse examples/test.om
test_cli_output "emit-ir-leading-opt" "i64 @main" 0 ./build/omsc -O3 emit-ir examples/exit_zero.om
test_cli_output "lex" "FN" 0 ./build/omsc lex examples/test.om
test_cli_output "lex-flag" "FN" 0 ./build/omsc --lex examples/test.om
test_cli_output "lex-compound-ops" "PLUS_ASSIGN" 0 ./build/omsc lex examples/compound_assign.om
test_cli_output "lex-do-while" "DO" 0 ./build/omsc lex examples/do_while.om
test_cli_output "lex-ternary" "QUESTION" 0 ./build/omsc lex examples/ternary.om
test_cli_output "lex-bitwise" "AMPERSAND" 0 ./build/omsc lex examples/bitwise.om
test_cli_output "lex-bool-true" "TRUE" 0 ./build/omsc lex examples/bool_test.om
test_cli_output "lex-bool-false" "FALSE" 0 ./build/omsc lex examples/bool_test.om
test_cli_output "lex-null" "NULL_LITERAL" 0 ./build/omsc lex examples/bool_test.om
test_cli_output "lex-bitwise-assign" "AMPERSAND_ASSIGN" 0 ./build/omsc lex examples/bitwise_assign_test.om
test_cli_output "tokens-flag" "FN" 0 ./build/omsc --tokens examples/test.om
test_cli_output "parse" "Parsed program" 0 ./build/omsc parse examples/test.om
test_cli_output "emit-ast-command" "Parsed program" 0 ./build/omsc emit-ast examples/test.om
test_cli_output "emit-ast-flag" "Parsed program" 0 ./build/omsc --emit-ast examples/test.om
test_cli_output "parse-flag" "Parsed program" 0 ./build/omsc --parse examples/test.om
test_cli_output "ast-flag" "Parsed program" 0 ./build/omsc --ast examples/test.om
test_cli_output "emit-ir" "i64 @main" 0 ./build/omsc --emit-ir examples/exit_zero.om
test_cli_output "emit-ir-alias" "i64 @main" 0 ./build/omsc --ir examples/exit_zero.om
test_cli_output "emit-ir-output-flag" "" 0 ./build/omsc emit-ir examples/exit_zero.om --output emit_ir_flag.ll
TOTAL=$((TOTAL + 1))
if [ ! -f emit_ir_flag.ll ] || ! grep -q "i64 @main" emit_ir_flag.ll; then
    echo -e "${RED}✗ Failed (emit-ir output flag did not write file)${NC}"
    FAILURES=$((FAILURES + 1))
fi
rm -f emit_ir_flag.ll
test_cli_output "unknown-command" "Error: unknown command" 1 ./build/omsc frob
test_cli_output "uninstall-not-installed" "not installed" 0 ./build/omsc uninstall
test_cli_output "uninstall-alias" "not installed" 0 ./build/omsc --uninstall
test_cli_output "help-shows-uninstall" "uninstall" 0 ./build/omsc --help
test_cli_output "unknown-option" "Error: unknown option '--bad-flag'" 1 ./build/omsc run examples/exit_zero.om --bad-flag
test_cli_output "output-unsupported-for-lex" "Error: -o/--output is only supported for compile/run/emit-ir/clean commands" 1 ./build/omsc lex examples/test.om -o /tmp/lex_out
test_cli_output "output-empty" "Error: -o/--output requires a valid output file name" 1 ./build/omsc run examples/exit_zero.om -o ""
test_cli_output "output-dash-invalid" "Error: -o/--output requires a valid output file name" 1 ./build/omsc run examples/exit_zero.om -o -bad
test_cli_output "output-missing-arg" "Error: -o/--output requires an argument" 1 ./build/omsc run examples/exit_zero.om -o
test_cli_output "output-multiple" "Error: output file specified multiple times" 1 ./build/omsc examples/exit_zero.om -o /tmp/out_a -o /tmp/out_b
test_cli_output "keep-temps-non-run" "Error: -k/--keep-temps is only supported for run commands" 1 ./build/omsc examples/exit_zero.om -k
test_cli_output "clean-input-rejected" "Error: clean does not accept input files" 1 ./build/omsc clean examples/test.om
test_cli_output "multiple-inputs" "Error: multiple input files specified" 1 ./build/omsc examples/test.om examples/factorial.om
test_cli_output "run-missing-input" "Error: no input file specified" 1 ./build/omsc run
test_cli_output "build-flag" "Compilation successful!" 0 ./build/omsc --build examples/exit_zero.om -o build_flag_test
TOTAL=$((TOTAL + 1))
if [ ! -f build_flag_test ]; then
    echo -e "${RED}✗ Failed (build flag did not create executable)${NC}"
    FAILURES=$((FAILURES + 1))
fi
rm -f build_flag_test
test_cli_output "run-success" "Compilation successful!" 0 ./build/omsc run examples/exit_zero.om
test_cli_output "run-flag" "Compilation successful!" 0 ./build/omsc --run examples/exit_zero.om
test_cli_output "run-with-args-delimiter" "Compilation successful!" 0 ./build/omsc run examples/exit_zero.om -- --bad-flag 123
test_cli_output "run" "Program exited with code 120" 120 ./build/omsc run examples/factorial.om
test_cli_output "print-output" "42" 0 ./build/omsc run examples/print_test.om
test_cli_output "float-print" "3.5" 5 ./build/omsc run examples/float_test.om
test_cli_output "string-var-print" "hello from variable" 0 ./build/omsc run examples/string_var_test.om
test_cli_output "string-param-print" "from param" 0 ./build/omsc run examples/string_param_test.om
test_cli_output "string-return-print" "hello world" 0 ./build/omsc run examples/string_param_test.om
TOTAL=$((TOTAL + 1))
if [ -f a.out ] || [ -f a.out.o ]; then
    echo -e "${RED}✗ Failed (temporary output files not cleaned)${NC}"
    rm -f a.out a.out.o
    FAILURES=$((FAILURES + 1))
fi
test_cli_output "run-keep-temps-long" "Compilation successful!" 0 ./build/omsc run --keep-temps examples/exit_zero.om
test_cli_output "run-keep-temps-short" "Compilation successful!" 0 ./build/omsc run -k examples/exit_zero.om
TOTAL=$((TOTAL + 1))
if [ ! -f a.out ]; then
    echo -e "${RED}✗ Failed (expected executable to remain with --keep-temps)${NC}"
    rm -f a.out
    FAILURES=$((FAILURES + 1))
fi
test_cli_output "clean" "Cleaned outputs" 0 ./build/omsc -C
TOTAL=$((TOTAL + 1))
if [ -f a.out ] || [ -f a.out.o ]; then
    echo -e "${RED}✗ Failed (clean did not remove outputs)${NC}"
    rm -f a.out a.out.o
    FAILURES=$((FAILURES + 1))
fi
test_cli_output "clean-noop" "Nothing to clean" 0 ./build/omsc clean
test_cli_output "build-custom-clean-target" "Compilation successful!" 0 ./build/omsc examples/exit_zero.om -o /tmp/omscript_clean_target
TOTAL=$((TOTAL + 1))
if [ ! -f /tmp/omscript_clean_target ]; then
    echo -e "${RED}✗ Failed (expected custom clean target executable)${NC}"
    FAILURES=$((FAILURES + 1))
fi
test_cli_output "clean-custom-output" "Cleaned outputs" 0 ./build/omsc clean -o /tmp/omscript_clean_target
TOTAL=$((TOTAL + 1))
if [ -f /tmp/omscript_clean_target ] || [ -f /tmp/omscript_clean_target.o ]; then
    echo -e "${RED}✗ Failed (clean -o did not remove custom outputs)${NC}"
    rm -f /tmp/omscript_clean_target /tmp/omscript_clean_target.o
    FAILURES=$((FAILURES + 1))
fi
echo ""

echo "Running test programs:"
echo "--------------------------------------------"
test_program "examples/factorial.om" 120
test_program "examples/fibonacci.om" 55
test_program "examples/arithmetic.om" 240
test_program "examples/neg_div_test.om" 6
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
test_program "examples/inlining_test.om" 52
test_program "examples/string_test.om" 0
test_program "examples/array_test.om" 245
test_program "examples/array_assign_test.om" 286
test_program "examples/multi_var_test.om" 63
test_program "examples/str_eq_test.om" 10
test_program "examples/foreach_test.om" 150
test_program "examples/foreach_break_test.om" 12
test_program "examples/string_func_test.om" 178
test_program "examples/stdlib_test.om" 66
test_program "examples/stdlib2_test.om" 255
test_program "examples/float_test.om" 5
test_program "examples/string_var_test.om" 0
test_program "examples/string_param_test.om" 0
test_program "examples/print_return_test.om" 0
test_program "examples/optmax_div_zero.om" 1
test_program "examples/forward_ref_test.om" 24
test_program "examples/stdlib_float_test.om" 29
test_program "examples/float_edge_cases.om" 14
test_program "examples/switch_test.om" 60
test_program "examples/switch_break_test.om" 159
test_program "examples/continue_in_switch.om" 4
test_program "examples/continue_in_switch_loop.om" 27
test_program "examples/typeof_assert_test.om" 1
test_program "examples/bool_test.om" 73
test_program "examples/bitwise_assign_test.om" 55
test_program "examples/array_compound_test.om" 164
test_program "examples/print_char_return_test.om" 1
test_program "examples/swap_oob.om" 134
test_program "examples/char_at_oob.om" 134
test_compile_fail "examples/const_fail.om"
test_compile_fail "examples/break_outside_loop.om"
test_compile_fail "examples/continue_outside_loop.om"
test_compile_fail "examples/continue_in_switch_no_loop.om"
test_compile_fail "examples/switch_float_case.om"
test_compile_fail "examples/undefined_var.om"
test_compile_fail "examples/int_overflow.om"
test_compile_fail "examples/no_main.om"
test_compile_fail "examples/dup_func.om"
test_compile_fail "examples/dup_param.om"
test_compile_fail "examples/missing_semicolon.om"
test_cli_output "error-line-info" "line" 1 ./build/omsc examples/undefined_var.om -o /tmp/test_err
test_cli_output "error-includes-filename" "undefined_var.om" 1 ./build/omsc examples/undefined_var.om -o /tmp/test_err
test_cli_output "missing-semicolon-msg" "Expected ';'" 1 ./build/omsc examples/missing_semicolon.om -o /tmp/test_semicolon
test_cli_output "int-overflow-msg" "Integer literal out of range" 1 ./build/omsc examples/int_overflow.om -o /tmp/test_overflow
test_cli_output "no-main-msg" "No 'main' function defined" 1 ./build/omsc examples/no_main.om -o /tmp/test_nomain
test_cli_output "dup-func-msg" "Duplicate function definition" 1 ./build/omsc examples/dup_func.om -o /tmp/test_dupfunc
test_cli_output "dup-param-msg" "Duplicate parameter name" 1 ./build/omsc examples/dup_param.om -o /tmp/test_dupparam
test_cli_output "switch-float-case-msg" "case value must be an integer constant, not a float" 1 ./build/omsc examples/switch_float_case.om -o /tmp/test_sfcase

echo ""
echo "============================================"
echo "Optimization Tests"
echo "============================================"
echo ""

# Test optimization level flags
test_cli_output "opt-O0-compile" "Compilation successful!" 0 ./build/omsc -O0 examples/exit_zero.om -o /tmp/test_o0
rm -f /tmp/test_o0 /tmp/test_o0.o
test_cli_output "opt-O1-compile" "Compilation successful!" 0 ./build/omsc -O1 examples/exit_zero.om -o /tmp/test_o1
rm -f /tmp/test_o1 /tmp/test_o1.o
test_cli_output "opt-O3-compile" "Compilation successful!" 0 ./build/omsc -O3 examples/exit_zero.om -o /tmp/test_o3
rm -f /tmp/test_o3 /tmp/test_o3.o
test_cli_output "opt-Ofast-compile" "Compilation successful!" 0 ./build/omsc -Ofast examples/exit_zero.om -o /tmp/test_ofast
rm -f /tmp/test_ofast /tmp/test_ofast.o
test_cli_output "opt-O0-emit-ir" "i64 @main" 0 ./build/omsc emit-ir -O0 examples/exit_zero.om
test_cli_output "opt-O3-emit-ir" "i64 @main" 0 ./build/omsc emit-ir -O3 examples/exit_zero.om
test_cli_output "opt-Ofast-emit-ir" "i64 @main" 0 ./build/omsc emit-ir -Ofast examples/exit_zero.om
test_cli_output "opt-O0-run" "Compilation successful!" 0 ./build/omsc run -O0 examples/exit_zero.om
test_cli_output "opt-Ofast-run" "Compilation successful!" 0 ./build/omsc run -Ofast examples/exit_zero.om
test_cli_output "opt-help-shows-flags" "-O0" 0 ./build/omsc --help
test_cli_output "opt-help-shows-ofast" "-Ofast" 0 ./build/omsc --help

TOTAL=$((TOTAL + 1))
echo -n "Testing with O3 optimization... "
./build/omsc examples/benchmark.om -o benchmark_o3 > /dev/null 2>&1
if [ $? -eq 0 ]; then
    ./benchmark_o3 > /dev/null 2>&1
    rm -f benchmark_o3 benchmark_o3.o
    echo -e "${GREEN}✓ O3 compilation successful${NC}"
else
    echo -e "${RED}✗ O3 compilation failed${NC}"
    FAILURES=$((FAILURES + 1))
fi

echo ""
echo "============================================"
echo "Target & Feature Flag Tests"
echo "============================================"
echo ""

# Test -march and -mtune flags
test_cli_output "march-native" "Compilation successful!" 0 ./build/omsc -march=native examples/exit_zero.om -o /tmp/test_march_native
rm -f /tmp/test_march_native /tmp/test_march_native.o
test_cli_output "march-x86-64" "Compilation successful!" 0 ./build/omsc -march=x86-64 examples/exit_zero.om -o /tmp/test_march_x86
rm -f /tmp/test_march_x86 /tmp/test_march_x86.o
test_cli_output "mtune-generic" "Compilation successful!" 0 ./build/omsc -mtune=generic examples/exit_zero.om -o /tmp/test_mtune_gen
rm -f /tmp/test_mtune_gen /tmp/test_mtune_gen.o
test_cli_output "march-mtune-combined" "Compilation successful!" 0 ./build/omsc -march=x86-64 -mtune=generic examples/exit_zero.om -o /tmp/test_march_mtune
rm -f /tmp/test_march_mtune /tmp/test_march_mtune.o

# Test feature toggle flags
test_cli_output "fno-pic" "Compilation successful!" 0 ./build/omsc -fno-pic examples/exit_zero.om -o /tmp/test_nopic
rm -f /tmp/test_nopic /tmp/test_nopic.o
test_cli_output "fpic" "Compilation successful!" 0 ./build/omsc -fpic examples/exit_zero.om -o /tmp/test_pic
rm -f /tmp/test_pic /tmp/test_pic.o
test_cli_output "ffast-math" "Compilation successful!" 0 ./build/omsc -ffast-math examples/exit_zero.om -o /tmp/test_fastmath
rm -f /tmp/test_fastmath /tmp/test_fastmath.o
test_cli_output "fno-fast-math" "Compilation successful!" 0 ./build/omsc -fno-fast-math examples/exit_zero.om -o /tmp/test_nofastmath
rm -f /tmp/test_nofastmath /tmp/test_nofastmath.o
test_cli_output "fno-optmax" "Compilation successful!" 0 ./build/omsc -fno-optmax examples/optmax.om -o /tmp/test_nooptmax
rm -f /tmp/test_nooptmax /tmp/test_nooptmax.o
test_cli_output "foptmax" "Compilation successful!" 0 ./build/omsc -foptmax examples/optmax.om -o /tmp/test_optmax
rm -f /tmp/test_optmax /tmp/test_optmax.o
test_cli_output "fno-jit" "Compilation successful!" 0 ./build/omsc -fno-jit examples/exit_zero.om -o /tmp/test_nojit
rm -f /tmp/test_nojit /tmp/test_nojit.o
test_cli_output "fjit" "Compilation successful!" 0 ./build/omsc -fjit examples/exit_zero.om -o /tmp/test_jit
rm -f /tmp/test_jit /tmp/test_jit.o
test_cli_output "flto" "Compilation successful!" 0 ./build/omsc -flto examples/exit_zero.om -o /tmp/test_lto
rm -f /tmp/test_lto /tmp/test_lto.o
test_cli_output "fno-lto" "Compilation successful!" 0 ./build/omsc -fno-lto examples/exit_zero.om -o /tmp/test_nolto
rm -f /tmp/test_nolto /tmp/test_nolto.o
test_cli_output "fstack-protector" "Compilation successful!" 0 ./build/omsc -fstack-protector examples/exit_zero.om -o /tmp/test_sp
rm -f /tmp/test_sp /tmp/test_sp.o
test_cli_output "fno-stack-protector" "Compilation successful!" 0 ./build/omsc -fno-stack-protector examples/exit_zero.om -o /tmp/test_nosp
rm -f /tmp/test_nosp /tmp/test_nosp.o
test_cli_output "strip-long" "Compilation successful!" 0 ./build/omsc --strip examples/exit_zero.om -o /tmp/test_strip
rm -f /tmp/test_strip /tmp/test_strip.o
test_cli_output "strip-short" "Compilation successful!" 0 ./build/omsc -s examples/exit_zero.om -o /tmp/test_strip_s
rm -f /tmp/test_strip_s /tmp/test_strip_s.o

# Test combined flags with run
test_cli_output "run-march-fno-jit" "Compilation successful!" 0 ./build/omsc run -march=native -fno-jit examples/exit_zero.om
test_cli_output "run-combined-flags" "Compilation successful!" 0 ./build/omsc run -O3 -march=x86-64 -ffast-math examples/exit_zero.om

# Test help output includes new flags
test_cli_output "help-shows-march" "-march=" 0 ./build/omsc --help
test_cli_output "help-shows-mtune" "-mtune=" 0 ./build/omsc --help
test_cli_output "help-shows-flto" "-flto" 0 ./build/omsc --help
test_cli_output "help-shows-fpic" "-fpic" 0 ./build/omsc --help
test_cli_output "help-shows-ffast-math" "-ffast-math" 0 ./build/omsc --help
test_cli_output "help-shows-foptmax" "-foptmax" 0 ./build/omsc --help
test_cli_output "help-shows-fjit" "-fjit" 0 ./build/omsc --help
test_cli_output "help-shows-static" "-static" 0 ./build/omsc --help
test_cli_output "help-shows-strip" "--strip" 0 ./build/omsc --help
test_cli_output "help-shows-fstack-protector" "-fstack-protector" 0 ./build/omsc --help

echo ""
echo "============================================"
if [ $FAILURES -eq 0 ]; then
    echo -e "${GREEN}All $TOTAL tests passed!${NC}"
else
    echo -e "${RED}$FAILURES of $TOTAL tests FAILED${NC}"
fi
echo "============================================"

exit $FAILURES
