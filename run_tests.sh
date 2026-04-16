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
cmake .. -DLLVM_DIR=$(/usr/lib/llvm-18/bin/llvm-config --cmakedir) > /dev/null 2>&1
make -j$(nproc) > /dev/null 2>&1

if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Build failed${NC}"
    exit 1
fi
echo -e "${GREEN}✓ Build successful${NC}"
echo ""

cd ..

# ── Parallel .om test infrastructure ──────────────────────────────────────────
# ptest_program / ptest_compile_fail submit background compile+run jobs.
# Results are written to per-test temp files.
# flush_ptests() waits for all pending jobs and prints results in order.
TMPTEST=$(mktemp -d)
trap 'rm -rf "$TMPTEST"' EXIT
_PT_IDX=0
declare -a _PT_PIDS=()
declare -a _PT_NAMES=()
MAX_PARALLEL=$(nproc)

# Throttle: wait for a slot when too many jobs are in flight
_ptest_throttle() {
    local active
    active=$(jobs -rp 2>/dev/null | wc -l)
    while (( active >= MAX_PARALLEL )); do
        wait -n 2>/dev/null || break
        active=$(jobs -rp 2>/dev/null | wc -l)
    done
}

ptest_program() {
    local source=$1
    local expected=$2
    local name; name=$(basename "$source" .om)
    local idx=$_PT_IDX
    _PT_IDX=$(( _PT_IDX + 1 ))
    _PT_NAMES[$idx]="$name"
    TOTAL=$(( TOTAL + 1 ))

    local exe="$TMPTEST/exe_${idx}"
    local rf="$TMPTEST/r_${idx}"
    (
        ./build/omsc "$source" -o "$exe" >/dev/null 2>&1 \
            || { echo "FAIL compile-failed" > "$rf"; exit 0; }
        timeout 60 "$exe" 2>/dev/null
        local rc=$?
        rm -f "$exe"
        local em=$(( expected % 256 ))
        if   [ "$rc" -eq 124 ]; then echo "FAIL timed-out"            > "$rf"
        elif [ "$rc" -eq "$em" ]; then echo "PASS $rc"                > "$rf"
        else                          echo "FAIL wrong-exit $em $rc"  > "$rf"
        fi
    ) 2>/dev/null &
    _PT_PIDS[$idx]=$!
    _ptest_throttle
}

ptest_compile_fail() {
    local source=$1
    local name; name=$(basename "$source" .om)
    local idx=$_PT_IDX
    _PT_IDX=$(( _PT_IDX + 1 ))
    _PT_NAMES[$idx]="$name (compile-fail)"
    TOTAL=$(( TOTAL + 1 ))

    local rf="$TMPTEST/r_${idx}"
    local tmp_out="$TMPTEST/cf_${idx}"
    (
        ./build/omsc "$source" -o "$tmp_out" >/dev/null 2>&1
        if [ $? -ne 0 ]; then
            echo "PASS expected-fail" > "$rf"
        else
            rm -f "$tmp_out"
            echo "FAIL unexpected-success" > "$rf"
        fi
    ) &
    _PT_PIDS[$idx]=$!
    _ptest_throttle
}

flush_ptests() {
    # Wait for every pending job, then print results in submission order.
    local total_idx=$_PT_IDX
    for i in $(seq 0 $(( total_idx - 1 ))); do
        local pid="${_PT_PIDS[$i]:-}"
        [ -n "$pid" ] && wait "$pid" 2>/dev/null || true
        local name="${_PT_NAMES[$i]:-unknown}"
        local rf="$TMPTEST/r_${i}"
        local line="FAIL no-result-file"
        [ -f "$rf" ] && line=$(cat "$rf")
        local status="${line%% *}"
        local detail="${line#* }"
        if [ "$status" = "PASS" ]; then
            echo -e "Testing ${name}... ${GREEN}✓ Passed${NC}"
        else
            echo -e "Testing ${name}... ${RED}✗ Failed (${detail})${NC}"
            FAILURES=$(( FAILURES + 1 ))
        fi
    done
    # Reset state for the next flush group
    _PT_IDX=0
    _PT_PIDS=()
    _PT_NAMES=()
}

# ptest_cli_output — parallel variant of test_cli_output.
# Usage: ptest_cli_output "name" "expected_substr" expected_exit cmd [args...]
# Submits a background job; call flush_ptests to collect results.
ptest_cli_output() {
    local name=$1
    local expected=$2
    local expected_exit=$3
    shift 3
    local idx=$_PT_IDX
    _PT_IDX=$(( _PT_IDX + 1 ))
    _PT_NAMES[$idx]="$name"
    TOTAL=$(( TOTAL + 1 ))

    local rf="$TMPTEST/r_${idx}"
    (
        local output
        output=$("$@" 2>&1)
        local rc=$?
        if [ "$rc" -ne "$expected_exit" ]; then
            echo "FAIL expected-exit-${expected_exit}-got-${rc}" > "$rf"
        elif [ -n "$expected" ] && ! echo "$output" | grep -qF -- "$expected"; then
            echo "FAIL missing-expected-output" > "$rf"
        else
            echo "PASS ok" > "$rf"
        fi
    ) &
    _PT_PIDS[$idx]=$!
    _ptest_throttle
}

# ── Sequential helpers (used for CLI / side-effect tests) ──────────────────────
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
test_cli_output "lex-hex-literal" "INTEGER '0xFF'" 0 ./build/omsc lex examples/hex_oct_bin_test.om
test_cli_output "lex-octal-literal" "INTEGER '0o77'" 0 ./build/omsc lex examples/hex_oct_bin_test.om
test_cli_output "lex-binary-literal" "INTEGER '0b1111'" 0 ./build/omsc lex examples/hex_oct_bin_test.om
test_cli_output "lex-bytes-literal" "BYTES_LITERAL" 0 ./build/omsc lex examples/bytes_literal_test.om
test_cli_output "lex-underscore-decimal" "INTEGER '1000000'" 0 ./build/omsc lex examples/underscore_num_test.om
test_cli_output "lex-underscore-hex" "INTEGER '0xFF00'" 0 ./build/omsc lex examples/underscore_num_test.om
test_cli_output "lex-power-operator" "STAR_STAR '**'" 0 ./build/omsc lex examples/power_operator_test.om
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
test_cli_output "help-shows-update" "update" 0 ./build/omsc --help
test_cli_output "update-command" "Detected distribution:" 0 ./build/omsc update
test_cli_output "update-flag" "Detected distribution:" 0 ./build/omsc --update
# Clean up any binary installed by the update tests so future runs start clean.
rm -f "$HOME/.local/bin/omsc"
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
test_cli_output "build-flag" "compiled" 0 ./build/omsc --build examples/exit_zero.om -o build_flag_test
TOTAL=$((TOTAL + 1))
if [ ! -f build_flag_test ]; then
    echo -e "${RED}✗ Failed (build flag did not create executable)${NC}"
    FAILURES=$((FAILURES + 1))
fi
rm -f build_flag_test
test_cli_output "run-success" "compiled" 0 ./build/omsc run examples/exit_zero.om
test_cli_output "run-flag" "compiled" 0 ./build/omsc --run examples/exit_zero.om
test_cli_output "run-with-args-delimiter" "compiled" 0 ./build/omsc run examples/exit_zero.om -- --bad-flag 123
test_cli_output "run" "Program exited with code 120" 120 ./build/omsc run examples/factorial.om
test_cli_output "print-output" "42" 0 ./build/omsc run examples/print_test.om
test_cli_output "float-print" "3.5" 5 ./build/omsc run examples/float_test.om
test_cli_output "string-var-print" "hello from variable" 0 ./build/omsc run examples/string_var_test.om
test_cli_output "string-param-print" "from param" 0 ./build/omsc run examples/string_param_test.om
test_cli_output "string-return-print" "hello world" 0 ./build/omsc run examples/string_param_test.om
test_cli_output "to-string-print" "12345" 87 ./build/omsc run examples/new_builtins_test.om
test_cli_output "null-coalesce-multiline" "multi-line works" 5 ./build/omsc run examples/null_coalesce_test.om
test_cli_output "lex-null-coalesce" "NULL_COALESCE" 0 ./build/omsc lex examples/null_coalesce_test.om
TOTAL=$((TOTAL + 1))
if [ -f a.out ] || [ -f a.out.o ]; then
    echo -e "${RED}✗ Failed (temporary output files not cleaned)${NC}"
    rm -f a.out a.out.o
    FAILURES=$((FAILURES + 1))
fi
test_cli_output "run-keep-temps-long" "compiled" 0 ./build/omsc run --keep-temps examples/exit_zero.om
test_cli_output "run-keep-temps-short" "compiled" 0 ./build/omsc run -k examples/exit_zero.om
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
test_cli_output "build-custom-clean-target" "compiled" 0 ./build/omsc examples/exit_zero.om -o /tmp/omscript_clean_target
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

echo "Running test programs (parallel, $(nproc) workers):"
echo "--------------------------------------------"
ptest_program "examples/factorial.om" 120
ptest_program "examples/fibonacci.om" 55
ptest_program "examples/arithmetic.om" 240
ptest_program "examples/neg_div_test.om" 6
ptest_program "examples/neg_pow_test.om" 11
ptest_program "examples/neg_shift_test.om" 0
ptest_program "examples/test.om" 84
ptest_program "examples/optimized_loops.om" 5040
ptest_program "examples/descending_range.om" 15
ptest_program "examples/advanced.om" 16
ptest_program "examples/break_continue.om" 10
ptest_program "examples/scoping.om" 5
ptest_program "examples/optmax.om" 10
ptest_program "examples/postfix.om" 4
ptest_program "examples/short_circuit.om" 1
ptest_program "examples/div_zero.om" 1
ptest_program "examples/mod_zero.om" 1
ptest_program "examples/refcount_test.om" 97
ptest_program "examples/compound_assign.om" 76
ptest_program "examples/block_comments.om" 30
ptest_program "examples/do_while.om" 16
ptest_program "examples/print_test.om" 0
ptest_program "examples/ternary.om" 34
ptest_program "examples/bitwise.om" 52
ptest_program "examples/prefix_ops.om" 50
ptest_program "examples/abs_test.om" 26
ptest_program "examples/optimization_stress_test.om" 432
ptest_program "examples/inlining_test.om" 52
ptest_program "examples/string_test.om" 0
ptest_program "examples/array_test.om" 245
ptest_program "examples/array_assign_test.om" 286
ptest_program "examples/multi_var_test.om" 63
ptest_program "examples/str_eq_test.om" 10
ptest_program "examples/foreach_test.om" 150
ptest_program "examples/foreach_break_test.om" 12
ptest_program "examples/string_func_test.om" 178
ptest_program "examples/stdlib_test.om" 66
ptest_program "examples/stdlib2_test.om" 194
ptest_program "examples/new_builtins_test.om" 87
ptest_program "examples/math_builtins_test.om" 52
ptest_program "examples/trig_math_test.om" 32
ptest_program "examples/string_builtins_test.om" 12
ptest_program "examples/char_predicates_test.om" 27
ptest_program "examples/array_builtins_test.om" 20
ptest_program "examples/array_utility_test.om" 18
ptest_program "examples/simd_register_test.om" 14
ptest_program "examples/register_test.om" 25
ptest_program "examples/compound_assign_test.om" 42
ptest_program "examples/null_coalesce_test.om" 5
ptest_program "examples/production_features_test.om" 14
ptest_program "examples/enum_test.om" 9
ptest_program "examples/default_params_test.om" 8
ptest_program "examples/array_copy_test.om" 21
ptest_program "examples/float_test.om" 5
ptest_program "examples/string_var_test.om" 0
ptest_program "examples/string_join_count_test.om" 41
ptest_program "examples/string_interp_test.om" 87
ptest_program "examples/multicase_interp_test.om" 110
ptest_program "examples/string_param_test.om" 0
ptest_program "examples/print_return_test.om" 0
ptest_program "examples/optmax_div_zero.om" 1
ptest_program "examples/forward_ref_test.om" 24
ptest_program "examples/stdlib_float_test.om" 29
ptest_program "examples/file_io_test.om" 8
ptest_program "examples/map_test.om" 18
ptest_program "examples/range_test.om" 21
ptest_program "examples/float_edge_cases.om" 14
ptest_program "examples/switch_test.om" 60
ptest_program "examples/switch_break_test.om" 159
ptest_program "examples/continue_in_switch.om" 4
ptest_program "examples/continue_in_switch_loop.om" 27
ptest_program "examples/typeof_assert_test.om" 1
ptest_program "examples/bool_test.om" 73
ptest_program "examples/bitwise_assign_test.om" 55
ptest_program "examples/array_compound_test.om" 164
ptest_program "examples/print_char_return_test.om" 1
ptest_program "examples/hex_oct_bin_test.om" 543
ptest_program "examples/bytes_literal_test.om" 56
ptest_program "examples/array_incdec_test.om" 162
ptest_program "examples/hex_escape_test.om" 0
ptest_program "examples/underscore_num_test.om" 178
ptest_program "examples/array_return_test.om" 110
ptest_program "examples/power_operator_test.om" 366
ptest_program "examples/str_concat_test.om" 22
ptest_program "examples/str_int_concat_test.om" 6
ptest_program "examples/memory_stress_test.om" 36
ptest_program "examples/constant_folding.om" 243
ptest_program "examples/ultimate_optimization.om" 184
ptest_program "examples/lambda_pipe_spread_test.om" 0
ptest_program "examples/array_higher_order_test.om" 0
ptest_program "examples/swap_oob.om" 134
ptest_program "examples/char_at_oob.om" 134
ptest_program "examples/overflow_wrap_test.om" 42
ptest_program "examples/benchmark_loops_math.om" 192
ptest_program "examples/try_catch_test.om" 88
ptest_program "examples/throw_in_called_fn_test.om" 147
ptest_program "examples/len_string_test.om" 8
ptest_program "examples/float_string_concat_test.om" 10
ptest_program "examples/typeof_full_test.om" 12
ptest_program "examples/string_index_test.om" 13
ptest_program "examples/string_foreach_test.om" 9
ptest_program "examples/to_int_to_float_string_test.om" 12
ptest_program "examples/string_index_assign_test.om" 11
ptest_program "examples/pow_float_test.om" 11
ptest_program "examples/str_replace_all_test.om" 10
ptest_program "examples/to_char_test.om" 12
ptest_program "examples/string_comparison_test.om" 15
ptest_program "examples/string_multiply_test.om" 9
ptest_program "examples/string_array_test.om" 20
ptest_program "examples/array_fill_negative_test.om" 3
ptest_program "examples/struct_test.om" 0
ptest_program "examples/import_test.om" 0
ptest_program "examples/generic_test.om" 0
ptest_program "examples/thread_test.om" 0
ptest_program "examples/string_fold_test.om" 0
ptest_program "examples/circular_import_test.om" 0
ptest_program "examples/hint_inline.om" 0
ptest_program "examples/hint_cold_hot.om" 0
ptest_program "examples/hint_pure_static.om" 0
ptest_program "examples/hint_prefetch.om" 0
ptest_program "examples/prefetch_use_site.om" 0
ptest_program "examples/freeze_test.om" 42
ptest_program "examples/borrow_mut_test.om" 7
ptest_program "examples/ownership_scope_test.om" 55
ptest_program "examples/hint_flatten.om" 0
ptest_program "examples/hint_vectorize.om" 0
ptest_program "examples/unsigned_types.om" 0
ptest_program "examples/precision_builtins.om" 0
ptest_program "examples/lambda_test.om" 10
ptest_program "examples/try_catch.om" 25
ptest_program "examples/type_annotation_test.om" 6
ptest_program "examples/lcm_test.om" 0
ptest_compile_fail "examples/const_fail.om"
ptest_compile_fail "examples/break_outside_loop.om"
ptest_compile_fail "examples/continue_outside_loop.om"
ptest_compile_fail "examples/continue_in_switch_no_loop.om"
ptest_compile_fail "examples/switch_float_case.om"
ptest_compile_fail "examples/undefined_var.om"
ptest_compile_fail "examples/int_overflow.om"
ptest_compile_fail "examples/no_main.om"
ptest_compile_fail "examples/dup_func.om"
ptest_compile_fail "examples/dup_param.om"
ptest_compile_fail "examples/dup_case.om"
ptest_compile_fail "examples/missing_semicolon.om"
ptest_compile_fail "examples/invalid_hex.om"
ptest_compile_fail "examples/invalid_binary.om"
ptest_compile_fail "examples/invalid_octal.om"
ptest_compile_fail "examples/unknown_annotation.om"
flush_ptests
test_cli_output "error-line-info" "line" 1 ./build/omsc examples/undefined_var.om -o /tmp/test_err
test_cli_output "error-includes-filename" "undefined_var.om" 1 ./build/omsc examples/undefined_var.om -o /tmp/test_err
test_cli_output "missing-semicolon-msg" "Expected ';'" 1 ./build/omsc examples/missing_semicolon.om -o /tmp/test_semicolon
test_cli_output "int-overflow-msg" "Integer literal out of range" 1 ./build/omsc examples/int_overflow.om -o /tmp/test_overflow
test_cli_output "no-main-msg" "No 'main' function defined" 1 ./build/omsc examples/no_main.om -o /tmp/test_nomain
test_cli_output "dup-func-msg" "Duplicate function definition" 1 ./build/omsc examples/dup_func.om -o /tmp/test_dupfunc
test_cli_output "dup-param-msg" "Duplicate parameter name" 1 ./build/omsc examples/dup_param.om -o /tmp/test_dupparam
test_cli_output "dup-case-msg" "duplicate case value" 1 ./build/omsc examples/dup_case.om -o /tmp/test_dupcase
test_cli_output "switch-float-case-msg" "case value must be an integer constant, not a float" 1 ./build/omsc examples/switch_float_case.om -o /tmp/test_sfcase
test_cli_output "invalid-hex-msg" "Expected hex digit after" 1 ./build/omsc examples/invalid_hex.om -o /tmp/test_hex_err
test_cli_output "invalid-binary-msg" "Expected binary digit after" 1 ./build/omsc examples/invalid_binary.om -o /tmp/test_bin_err
test_cli_output "invalid-octal-msg" "Expected octal digit after" 1 ./build/omsc examples/invalid_octal.om -o /tmp/test_oct_err

echo ""
echo "============================================"
echo "Optimization Tests (parallel)"
echo "============================================"
echo ""

# Test optimization level flags (parallel — each job gets a unique tmp exe)
ptest_cli_output "opt-O0-compile" "compiled" 0 ./build/omsc -O0 examples/exit_zero.om -o "$TMPTEST/t_o0"
ptest_cli_output "opt-O1-compile" "compiled" 0 ./build/omsc -O1 examples/exit_zero.om -o "$TMPTEST/t_o1"
ptest_cli_output "opt-O3-compile" "compiled" 0 ./build/omsc -O3 examples/exit_zero.om -o "$TMPTEST/t_o3"
ptest_cli_output "opt-Ofast-compile" "compiled" 0 ./build/omsc -Ofast examples/exit_zero.om -o "$TMPTEST/t_ofast"
ptest_cli_output "opt-O0-emit-ir" "i64 @main" 0 ./build/omsc emit-ir -O0 examples/exit_zero.om
ptest_cli_output "opt-O3-emit-ir" "i64 @main" 0 ./build/omsc emit-ir -O3 examples/exit_zero.om
ptest_cli_output "opt-Ofast-emit-ir" "i64 @main" 0 ./build/omsc emit-ir -Ofast examples/exit_zero.om
ptest_cli_output "opt-help-shows-flags" "-O0" 0 ./build/omsc --help
flush_ptests

# run subcommand tests must be sequential: omsc run spawns a child process
# that can receive SIGHUP when executed inside a background subshell.
test_cli_output "opt-O0-run" "compiled" 0 ./build/omsc run -O0 examples/exit_zero.om
test_cli_output "opt-Ofast-run" "compiled" 0 ./build/omsc run -Ofast examples/exit_zero.om

TOTAL=$((TOTAL + 1))
echo -n "Testing with O3 optimization... "
./build/omsc examples/benchmark.om -o "$TMPTEST/benchmark_o3" > /dev/null 2>&1
if [ $? -eq 0 ]; then
    "$TMPTEST/benchmark_o3" > /dev/null 2>&1
    echo -e "${GREEN}✓ O3 compilation successful${NC}"
else
    echo -e "${RED}✗ O3 compilation failed${NC}"
    FAILURES=$((FAILURES + 1))
fi

echo ""
echo "============================================"
echo "Target & Feature Flag Tests (parallel)"
echo "============================================"
echo ""

# Test -march and -mtune flags (parallel)
ptest_cli_output "march-native" "compiled" 0 ./build/omsc -march=native examples/exit_zero.om -o "$TMPTEST/t_march_native"
ptest_cli_output "march-x86-64" "compiled" 0 ./build/omsc -march=x86-64 examples/exit_zero.om -o "$TMPTEST/t_march_x86"
ptest_cli_output "mtune-generic" "compiled" 0 ./build/omsc -mtune=generic examples/exit_zero.om -o "$TMPTEST/t_mtune_gen"
ptest_cli_output "march-mtune-combined" "compiled" 0 ./build/omsc -march=x86-64 -mtune=generic examples/exit_zero.om -o "$TMPTEST/t_march_mtune"

# Test feature toggle flags (parallel)
ptest_cli_output "fno-pic" "compiled" 0 ./build/omsc -fno-pic examples/exit_zero.om -o "$TMPTEST/t_nopic"
ptest_cli_output "fpic" "compiled" 0 ./build/omsc -fpic examples/exit_zero.om -o "$TMPTEST/t_pic"
ptest_cli_output "ffast-math" "compiled" 0 ./build/omsc -ffast-math examples/exit_zero.om -o "$TMPTEST/t_fastmath"
ptest_cli_output "fno-fast-math" "compiled" 0 ./build/omsc -fno-fast-math examples/exit_zero.om -o "$TMPTEST/t_nofastmath"
ptest_cli_output "fno-optmax" "compiled" 0 ./build/omsc -fno-optmax examples/optmax.om -o "$TMPTEST/t_nooptmax"
ptest_cli_output "foptmax" "compiled" 0 ./build/omsc -foptmax examples/optmax.om -o "$TMPTEST/t_optmax"
ptest_cli_output "flto" "compiled" 0 ./build/omsc -flto examples/exit_zero.om -o "$TMPTEST/t_lto"
ptest_cli_output "fno-lto" "compiled" 0 ./build/omsc -fno-lto examples/exit_zero.om -o "$TMPTEST/t_nolto"
ptest_cli_output "fstack-protector" "compiled" 0 ./build/omsc -fstack-protector examples/exit_zero.om -o "$TMPTEST/t_sp"
ptest_cli_output "fno-stack-protector" "compiled" 0 ./build/omsc -fno-stack-protector examples/exit_zero.om -o "$TMPTEST/t_nosp"
ptest_cli_output "strip-long" "compiled" 0 ./build/omsc --strip examples/exit_zero.om -o "$TMPTEST/t_strip"
ptest_cli_output "strip-short" "compiled" 0 ./build/omsc -s examples/exit_zero.om -o "$TMPTEST/t_strip_s"

# run with combined flags must be sequential (same SIGHUP reason as above)
test_cli_output "run-combined-flags" "compiled" 0 ./build/omsc run -O3 -march=x86-64 -ffast-math examples/exit_zero.om

# Test help output includes new flags (parallel)
ptest_cli_output "help-shows-march" "-march=" 0 ./build/omsc --help
ptest_cli_output "help-shows-mtune" "-mtune=" 0 ./build/omsc --help
ptest_cli_output "help-shows-flto" "-flto" 0 ./build/omsc --help
ptest_cli_output "help-shows-fpic" "-fpic" 0 ./build/omsc --help
ptest_cli_output "help-shows-ffast-math" "-ffast-math" 0 ./build/omsc --help
ptest_cli_output "help-shows-foptmax" "-foptmax" 0 ./build/omsc --help
ptest_cli_output "help-shows-static" "-static" 0 ./build/omsc --help
ptest_cli_output "help-shows-strip" "--strip" 0 ./build/omsc --help
ptest_cli_output "help-shows-fstack-protector" "-fstack-protector" 0 ./build/omsc --help
flush_ptests

echo ""
echo "============================================"
echo "New CLI Feature Tests (parallel)"
echo "============================================"
echo ""

# check command
ptest_cli_output "check-valid" "OK" 0 ./build/omsc check examples/factorial.om
ptest_cli_output "check-syntax-error" "Expected ';'" 1 ./build/omsc check examples/missing_semicolon.om
ptest_cli_output "check-flag" "OK" 0 ./build/omsc --check examples/exit_zero.om

# --quiet flag
ptest_cli_output "quiet-check" "" 0 ./build/omsc -q check examples/factorial.om
ptest_cli_output "quiet-long" "" 0 ./build/omsc --quiet check examples/factorial.om

# --time flag
ptest_cli_output "time-check" "Timing:" 0 ./build/omsc check examples/factorial.om --time
ptest_cli_output "time-lex" "Timing:" 0 ./build/omsc lex examples/exit_zero.om --time
ptest_cli_output "time-parse" "Timing:" 0 ./build/omsc parse examples/exit_zero.om --time
ptest_cli_output "time-compile" "Timing:" 0 ./build/omsc --time examples/exit_zero.om -o "$TMPTEST/t_time"

# --dump-ast flag
ptest_cli_output "dump-ast" "FunctionDecl" 0 ./build/omsc parse examples/exit_zero.om --dump-ast
ptest_cli_output "dump-ast-shows-return" "ReturnStmt" 0 ./build/omsc parse examples/exit_zero.om --dump-ast
ptest_cli_output "dump-ast-shows-block" "Block" 0 ./build/omsc parse examples/exit_zero.om --dump-ast

# --dump-tokens alias
ptest_cli_output "dump-tokens-alias" "FN" 0 ./build/omsc --dump-tokens examples/test.om

# --dry-run flag (output checks — parallel)
ptest_cli_output "dry-run-compile" "Dry run" 0 ./build/omsc --dry-run examples/factorial.om
ptest_cli_output "dry-run-no-output-file" "" 0 ./build/omsc --dry-run examples/exit_zero.om
ptest_cli_output "dry-run-invalid" "Unknown variable" 1 ./build/omsc --dry-run examples/undefined_var.om
ptest_cli_output "dry-run-emit-ir" "Dry run" 0 ./build/omsc --dry-run emit-ir examples/exit_zero.om
flush_ptests

# dry-run file-existence check must run after flush (sequential, file-system side-effect)
TOTAL=$((TOTAL + 1))
echo -n "Testing dry-run-no-binary-created... "
./build/omsc --dry-run examples/exit_zero.om > /dev/null 2>&1
if [ ! -f a.out ]; then
    echo -e "${GREEN}✓ Passed${NC}"
else
    echo -e "${RED}✗ Failed (binary should not be created in dry-run mode)${NC}"
    rm -f a.out
    FAILURES=$((FAILURES + 1))
fi

# --emit-obj flag (output check parallel, file-existence check sequential)
ptest_cli_output "emit-obj" "Object file written" 0 ./build/omsc --emit-obj examples/exit_zero.om -o "$TMPTEST/omsc_obj_test.o"
ptest_cli_output "emit-obj-default-name" "exit_zero.o" 0 ./build/omsc --emit-obj examples/exit_zero.om
flush_ptests

TOTAL=$((TOTAL + 1))
echo -n "Testing emit-obj-file-exists... "
# Re-run to get the file into a predictable location for the existence check
./build/omsc --emit-obj examples/exit_zero.om -o "$TMPTEST/omsc_obj_check.o" > /dev/null 2>&1
if [ -f "$TMPTEST/omsc_obj_check.o" ]; then
    echo -e "${GREEN}✓ Passed${NC}"
else
    echo -e "${RED}✗ Failed (object file should exist)${NC}"
    FAILURES=$((FAILURES + 1))
fi
rm -f exit_zero.o

# help / version (parallel)
ptest_cli_output "help-shows-check" "check" 0 ./build/omsc --help
ptest_cli_output "help-shows-time" "--time" 0 ./build/omsc --help
ptest_cli_output "help-shows-emit-obj" "--emit-obj" 0 ./build/omsc --help
ptest_cli_output "help-shows-dry-run" "--dry-run" 0 ./build/omsc --help
ptest_cli_output "help-shows-quiet" "--quiet" 0 ./build/omsc --help
ptest_cli_output "version-full-semver" "OmScript Compiler v" 0 ./build/omsc --version
flush_ptests

echo ""
echo "============================================"
echo "Package Manager Tests"
echo "============================================"
echo ""

# Clean up any leftover packages
rm -rf om_packages

# Start a local HTTP server to serve user-packages/ for testing.
# This exercises the real download path without requiring internet access.
PKG_SERVER_PORT=18923
PKG_SERVER_PID=""
if command -v python3 > /dev/null 2>&1; then
    python3 -m http.server $PKG_SERVER_PORT --directory user-packages > /dev/null 2>&1 &
    PKG_SERVER_PID=$!
    sleep 1
    export OMSC_REGISTRY_URL="http://127.0.0.1:${PKG_SERVER_PORT}"
    echo "Registry served at $OMSC_REGISTRY_URL (PID $PKG_SERVER_PID)"
else
    echo "Warning: python3 not found, using GitHub URLs for pkg tests"
fi

cleanup_pkg_server() {
    if [ -n "$PKG_SERVER_PID" ]; then
        kill $PKG_SERVER_PID 2>/dev/null
        wait $PKG_SERVER_PID 2>/dev/null
    fi
}
trap cleanup_pkg_server EXIT

# Help text shows package manager
test_cli_output "help-shows-pkg" "pkg" 0 ./build/omsc --help

# pkg search - list all packages
test_cli_output "pkg-search-all" "math@" 0 ./build/omsc pkg search
test_cli_output "pkg-search-algorithms" "algorithms@" 0 ./build/omsc pkg search
test_cli_output "pkg-search-strings" "strings@" 0 ./build/omsc pkg search

# pkg search with query
test_cli_output "pkg-search-query" "math@" 0 ./build/omsc pkg search math
test_cli_output "pkg-search-no-match" "No packages matching" 0 ./build/omsc pkg search zzzznonexistent

# pkg info
test_cli_output "pkg-info-math" "math" 0 ./build/omsc pkg info math
test_cli_output "pkg-info-version" "1.0.0" 0 ./build/omsc pkg info math
test_cli_output "pkg-info-not-installed" "Installed:   no" 0 ./build/omsc pkg info math
test_cli_output "pkg-info-nonexistent" "not found" 1 ./build/omsc pkg info nonexistent

# pkg list empty
test_cli_output "pkg-list-empty" "No packages installed" 0 ./build/omsc pkg list

# pkg install (downloads from local server)
test_cli_output "pkg-install-math" "Installed math@1.0.0" 0 ./build/omsc pkg install math
TOTAL=$((TOTAL + 1))
echo -n "Testing pkg-install-creates-files... "
if [ -f om_packages/math/math.om ] && [ -f om_packages/math/package.json ]; then
    echo -e "${GREEN}✓ Passed${NC}"
else
    echo -e "${RED}✗ Failed (package files not created)${NC}"
    FAILURES=$((FAILURES + 1))
fi

# pkg list after install
test_cli_output "pkg-list-installed" "math@1.0.0" 0 ./build/omsc pkg list

# pkg info shows installed
test_cli_output "pkg-info-installed" "Installed:   yes" 0 ./build/omsc pkg info math

# pkg install another
test_cli_output "pkg-install-algorithms" "Installed algorithms@1.0.0" 0 ./build/omsc pkg install algorithms

# pkg list shows both
test_cli_output "pkg-list-multiple" "algorithms@" 0 ./build/omsc pkg list

# pkg remove
test_cli_output "pkg-remove-math" "Removed math" 0 ./build/omsc pkg remove math
TOTAL=$((TOTAL + 1))
echo -n "Testing pkg-remove-deletes-files... "
if [ ! -d om_packages/math ]; then
    echo -e "${GREEN}✓ Passed${NC}"
else
    echo -e "${RED}✗ Failed (package directory still exists)${NC}"
    FAILURES=$((FAILURES + 1))
fi

# pkg remove nonexistent
test_cli_output "pkg-remove-not-installed" "not installed" 1 ./build/omsc pkg remove nonexistent

# Error cases
test_cli_output "pkg-no-subcommand" "missing pkg subcommand" 1 ./build/omsc pkg
test_cli_output "pkg-unknown-subcommand" "unknown pkg subcommand" 1 ./build/omsc pkg frob
test_cli_output "pkg-install-no-name" "requires a package name" 1 ./build/omsc pkg install
test_cli_output "pkg-remove-no-name" "requires a package name" 1 ./build/omsc pkg remove
test_cli_output "pkg-info-no-name" "requires a package name" 1 ./build/omsc pkg info
test_cli_output "pkg-install-nonexistent" "not found" 1 ./build/omsc pkg install nonexistent

# Aliases
test_cli_output "pkg-add-alias" "Installed strings@" 0 ./build/omsc pkg add strings
test_cli_output "pkg-ls-alias" "strings@" 0 ./build/omsc pkg ls
test_cli_output "pkg-rm-alias" "Removed" 0 ./build/omsc pkg rm strings
test_cli_output "pkg-find-alias" "math@" 0 ./build/omsc pkg find math
test_cli_output "pkg-show-alias" "math" 0 ./build/omsc pkg show math
test_cli_output "package-command-alias" "algorithms@" 0 ./build/omsc package list

# Quiet mode
test_cli_output "pkg-install-quiet" "" 0 ./build/omsc -q pkg install strings
test_cli_output "pkg-remove-quiet" "" 0 ./build/omsc -q pkg remove strings

# Clean up
rm -rf om_packages

# Optimization correctness tests + syntactic features + new features (parallel)
echo ""
echo "Running optimization and feature tests (parallel, $(nproc) workers):"
echo "--------------------------------------------"
ptest_program "examples/runtime_div_opt_test.om" 5
ptest_program "examples/loop_ucmp_test.om" 5
ptest_program "examples/dict_literal_test.om" 18
ptest_program "examples/preprocessor_test.om" 12
ptest_program "examples/dict_chaining_test.om" 16
ptest_program "examples/ifelse_select_test.om" 37
ptest_program "examples/const_float_fold_test.om" 7
ptest_program "examples/inverse_op_test.om" 59
ptest_program "examples/musttail_test.om" 175
ptest_program "examples/string_interning_test.om" 15
ptest_program "examples/constfold_builtins_test.om" 15
ptest_program "examples/cttz_loop_test.om" 6

# Syntactic features tests
ptest_program "examples/unless_test.om" 31
ptest_program "examples/until_test.om" 16
ptest_program "examples/loop_keyword_test.om" 25
ptest_program "examples/repeat_test.om" 65
ptest_program "examples/defer_test.om" 31
ptest_program "examples/guard_test.om" 170
ptest_program "examples/when_test.om" 65
ptest_program "examples/forever_test.om" 103
ptest_program "examples/foreach_keyword_test.om" 33
ptest_program "examples/do_until_test.om" 217
ptest_program "examples/elif_test.om" 165
ptest_program "examples/swap_test.om" 108
ptest_program "examples/times_test.om" 162
ptest_program "examples/pipeline_test.om" 131
ptest_program "examples/pipeline_cancel_test.om" 46
ptest_program "examples/destructure_test.om" 13130
ptest_program "examples/indexed_foreach_test.om" 1652
ptest_program "examples/repeat_until_test.om" 297
ptest_program "examples/step_downto_test.om" 112
ptest_program "examples/with_test.om" 188
ptest_program "examples/loop_counted_test.om" 63
ptest_program "examples/expr_fn_swap_test.om" 529
ptest_program "examples/assume_unreachable_expect_test.om" 125
ptest_program "examples/array_string_builtins_test.om" 178
ptest_program "examples/method_call_test.om" 129

# New syntax and optimization features
ptest_program "examples/scientific_notation_test.om" 7
ptest_program "examples/scope_resolution_test.om" 11
ptest_program "examples/slice_syntax_test.om" 11
ptest_program "examples/in_operator_test.om" 5
ptest_program "examples/logical_assign_test.om" 8
ptest_program "examples/elvis_operator_test.om" 5
ptest_program "examples/chained_comparison_test.om" 7
ptest_program "examples/large_int_literal_test.om" 7
ptest_program "examples/comptime_test.om" 172
ptest_program "examples/parallel_loop_test.om" 70
ptest_program "examples/independent_loop_test.om" 72
ptest_program "examples/loop_fuse_test.om" 35
ptest_program "examples/escape_analysis_test.om" 150
ptest_program "examples/freeze_correctness_test.om" 42
ptest_program "examples/reborrow_test.om" 100
ptest_program "examples/allocator_annotation_test.om" 40
ptest_program "examples/borrow_mut_syntax_test.om" 42
ptest_program "examples/comptime_chain_test.om" 45
ptest_program "examples/comptime_array_test.om" 478546148456
ptest_program "examples/comptime_array_append_test.om" 18
ptest_program "examples/comptime_loop_reason_test.om" 1
ptest_program "examples/loop_annotation_unroll_test.om" 120
ptest_program "examples/loop_annotation_vectorize_test.om" 36
flush_ptests

echo ""
echo "============================================"
if [ $FAILURES -eq 0 ]; then
    echo -e "${GREEN}All $TOTAL tests passed!${NC}"
else
    echo -e "${RED}$FAILURES of $TOTAL tests FAILED${NC}"
fi
echo "============================================"

exit $FAILURES
