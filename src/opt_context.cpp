/// @file opt_context.cpp
/// @brief Implementation of BuiltinEffectTable and OptimizationContext helpers.

#include "opt_context.h"
#include <unordered_map>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// BuiltinEffectTable — master builtin classification
// ─────────────────────────────────────────────────────────────────────────────
//
// This is THE canonical source of truth for the purity and side-effect
// classification of every OmScript built-in function.  It replaces the
// five previously-scattered local tables:
//
//   kPureBuiltins      in autoDetectConstEvalFunctions()  → constFoldable
//   kImpureBuiltins    in autoDetectConstEvalFunctions()  → writesMemory || hasIO
//   kIOBuiltins        in inferFunctionEffects()          → hasIO
//   kMutatingBuiltins  in inferFunctionEffects()          → writesMemory (no IO)
//   kReadBuiltins      in inferFunctionEffects()          → readsMemory (no write)
//
// A single edit here propagates to autoDetectConstEvalFunctions,
// inferFunctionEffects, and any future consumer.
//
// Column meanings:
//   {constFoldable, readsMemory, writesMemory, hasIO}
//
// Legend:
//   CF — const-foldable: safe to evaluate at compile time
//   RM — reads heap/array/string memory
//   WM — writes heap memory
//   IO — performs observable I/O

const std::unordered_map<std::string, BuiltinEffects>& BuiltinEffectTable::table() noexcept {
    // clang-format off
    static const std::unordered_map<std::string, BuiltinEffects> kTable = {
        //                               CF     RM     WM     IO

        // ── Pure arithmetic / math ────────────────────────────────────────
        {"abs",             {true,  false, false, false}},
        {"min",             {true,  false, false, false}},
        {"max",             {true,  false, false, false}},
        {"sign",            {true,  false, false, false}},
        {"clamp",           {true,  false, false, false}},
        {"pow",             {true,  false, false, false}},
        {"sqrt",            {true,  false, false, false}},
        {"log2",            {true,  false, false, false}},
        {"log",             {true,  false, false, false}},
        {"log10",           {true,  false, false, false}},
        {"exp",             {true,  false, false, false}},
        {"exp2",            {true,  false, false, false}},
        {"sin",             {true,  false, false, false}},
        {"cos",             {true,  false, false, false}},
        {"tan",             {true,  false, false, false}},
        {"asin",            {true,  false, false, false}},
        {"acos",            {true,  false, false, false}},
        {"atan",            {true,  false, false, false}},
        {"atan2",           {true,  false, false, false}},
        {"cbrt",            {true,  false, false, false}},
        {"hypot",           {true,  false, false, false}},
        {"fma",             {true,  false, false, false}},
        {"copysign",        {true,  false, false, false}},
        {"min_float",       {true,  false, false, false}},
        {"max_float",       {true,  false, false, false}},
        {"floor",           {true,  false, false, false}},
        {"ceil",            {true,  false, false, false}},
        {"round",           {true,  false, false, false}},
        {"gcd",             {true,  false, false, false}},
        {"lcm",             {true,  false, false, false}},
        {"fast_add",        {true,  false, false, false}},
        {"fast_sub",        {true,  false, false, false}},
        {"fast_mul",        {true,  false, false, false}},
        {"fast_div",        {true,  false, false, false}},
        {"precise_add",     {true,  false, false, false}},
        {"precise_sub",     {true,  false, false, false}},
        {"precise_mul",     {true,  false, false, false}},
        {"precise_div",     {true,  false, false, false}},

        // ── Pure bitwise / integer ops ────────────────────────────────────
        {"popcount",        {true,  false, false, false}},
        {"clz",             {true,  false, false, false}},
        {"ctz",             {true,  false, false, false}},
        {"bswap",           {true,  false, false, false}},
        {"bitreverse",      {true,  false, false, false}},
        {"rotate_left",     {true,  false, false, false}},
        {"rotate_right",    {true,  false, false, false}},
        {"saturating_add",  {true,  false, false, false}},
        {"saturating_sub",  {true,  false, false, false}},
        {"is_even",         {true,  false, false, false}},
        {"is_odd",          {true,  false, false, false}},
        {"is_power_of_2",   {true,  false, false, false}},

        // ── Pure character ops ────────────────────────────────────────────
        {"is_alpha",        {true,  false, false, false}},
        {"is_digit",        {true,  false, false, false}},
        {"is_upper",        {true,  false, false, false}},
        {"is_lower",        {true,  false, false, false}},
        {"is_space",        {true,  false, false, false}},
        {"is_alnum",        {true,  false, false, false}},
        {"to_char",         {true,  false, false, false}},
        {"char_code",       {true,  false, false, false}},

        // ── Pure type casts ───────────────────────────────────────────────
        {"to_int",          {true,  false, false, false}},
        {"to_float",        {true,  false, false, false}},
        {"to_string",       {true,  false, false, false}},
        {"number_to_string",{true,  false, false, false}},
        {"string_to_number",{true,  false, false, false}},
        {"str_to_int",      {true,  false, false, false}},
        {"u64",             {true,  false, false, false}},
        {"i64",             {true,  false, false, false}},
        {"int",             {true,  false, false, false}},
        {"uint",            {true,  false, false, false}},
        {"u32",             {true,  false, false, false}},
        {"i32",             {true,  false, false, false}},
        {"u16",             {true,  false, false, false}},
        {"i16",             {true,  false, false, false}},
        {"u8",              {true,  false, false, false}},
        {"i8",              {true,  false, false, false}},
        {"bool",            {true,  false, false, false}},
        {"typeof",          {true,  false, false, false}},

        // ── Pure string comparisons / predicates (read-only, const-foldable)
        {"str_eq",          {true,  true,  false, false}},
        {"str_find",        {true,  true,  false, false}},
        {"str_index_of",    {true,  true,  false, false}},
        {"str_starts_with", {true,  true,  false, false}},
        {"str_ends_with",   {true,  true,  false, false}},
        {"startswith",      {true,  true,  false, false}},
        {"endswith",        {true,  true,  false, false}},
        {"str_contains",    {true,  true,  false, false}},
        {"str_count",       {true,  true,  false, false}},
        {"char_at",         {true,  true,  false, false}},

        // ── String length (const-foldable + reads) ────────────────────────
        {"str_len",         {true,  true,  false, false}},
        {"len",             {true,  true,  false, false}},

        // ── String transformations (allocate new; const-foldable + reads) ─
        {"str_upper",       {true,  true,  false, false}},
        {"str_lower",       {true,  true,  false, false}},
        {"str_trim",        {true,  true,  false, false}},
        {"str_reverse",     {true,  true,  false, false}},
        {"str_substr",      {true,  true,  false, false}},
        {"str_replace",     {true,  true,  false, false}},
        {"str_repeat",      {true,  true,  false, false}},
        {"str_pad_left",    {true,  true,  false, false}},
        {"str_pad_right",   {true,  true,  false, false}},
        {"str_concat",      {true,  true,  false, false}},
        {"str_chars",       {true,  true,  false, false}},
        {"str_join",        {true,  true,  false, false}},
        {"str_split",       {true,  true,  false, false}},

        // ── Array reads / aggregations (const-foldable + reads) ───────────
        {"sum",             {true,  true,  false, false}},
        {"array_product",   {true,  true,  false, false}},
        {"array_last",      {true,  true,  false, false}},
        {"array_min",       {true,  true,  false, false}},
        {"array_max",       {true,  true,  false, false}},
        {"array_contains",  {true,  true,  false, false}},
        {"index_of",        {true,  true,  false, false}},
        {"array_find",      {true,  true,  false, false}},
        {"array_count",     {true,  true,  false, false}},
        {"array_any",       {true,  true,  false, false}},
        {"array_every",     {true,  true,  false, false}},
        {"array_reduce",    {false, true,  false, false}},  // higher-order: not CF
        {"array_map",       {false, true,  false, false}},  // higher-order: not CF
        {"array_filter",    {false, true,  false, false}},  // higher-order: not CF

        // ── Array constructors (const-foldable; allocate new) ─────────────
        {"array_fill",      {true,  false, false, false}},
        {"array_concat",    {true,  true,  false, false}},
        {"array_slice",     {true,  true,  false, false}},
        {"array_copy",      {true,  true,  false, false}},
        {"range",           {true,  false, false, false}},
        {"range_step",      {true,  false, false, false}},

        // ── Map reads (read-only, NOT const-foldable at compile time) ─────
        {"map_get",         {false, true,  false, false}},
        {"map_has",         {false, true,  false, false}},
        {"map_keys",        {false, true,  false, false}},
        {"map_values",      {false, true,  false, false}},
        {"map_size",        {false, true,  false, false}},

        // ── Mutating builtins (write memory, no I/O) ──────────────────────
        {"push",            {false, true,  true,  false}},
        {"pop",             {false, true,  true,  false}},
        {"sort",            {false, true,  true,  false}},
        {"reverse",         {false, true,  true,  false}},
        {"swap",            {false, true,  true,  false}},
        {"shuffle",         {false, true,  true,  false}},
        {"resize",          {false, true,  true,  false}},
        {"clear",           {false, true,  true,  false}},
        {"append",          {false, true,  true,  false}},
        {"insert",          {false, true,  true,  false}},
        {"remove",          {false, true,  true,  false}},
        {"array_remove",    {false, true,  true,  false}},
        {"array_insert",    {false, true,  true,  false}},
        {"map_set",         {false, true,  true,  false}},
        {"map_remove",      {false, true,  true,  false}},

        // ── I/O builtins ──────────────────────────────────────────────────
        {"print",           {false, false, false, true}},
        {"println",         {false, false, false, true}},
        {"write",           {false, false, false, true}},
        {"printf",          {false, false, false, true}},
        {"print_char",      {false, false, false, true}},
        {"input",           {false, false, false, true}},
        {"input_line",      {false, false, false, true}},
        {"file_read",       {false, false, false, true}},
        {"file_write",      {false, false, false, true}},
        {"file_append",     {false, false, false, true}},
        {"file_exists",     {false, false, false, true}},
        {"fopen",           {false, false, false, true}},
        {"fclose",          {false, false, false, true}},
        {"fread",           {false, false, false, true}},
        {"fwrite",          {false, false, false, true}},
        {"fappend",         {false, false, false, true}},
        {"thread_create",   {false, false, false, true}},
        {"thread_join",     {false, false, false, true}},
        {"mutex_lock",      {false, false, false, true}},
        {"mutex_unlock",    {false, false, false, true}},
        {"mutex_new",       {false, false, false, true}},
        {"mutex_destroy",   {false, false, false, true}},
        {"sleep",           {false, false, false, true}},
        {"exit",            {false, false, false, true}},
        {"exit_program",    {false, false, false, true}},
        {"abort",           {false, false, false, true}},
        {"panic",           {false, false, false, true}},
        {"error",           {false, false, false, true}},
        {"assert",          {false, false, false, true}},
        // ── Region management builtins ────────────────────────────────────────
        {"newRegion",       {false, false, true,  false}},
        {"alloc",           {false, false, true,  false}},
        {"random",          {false, false, false, true}},
        {"rand",            {false, false, false, true}},
        {"time",            {false, false, false, true}},
    };
    // clang-format on
    return kTable;
}

const BuiltinEffects& BuiltinEffectTable::get(const std::string& name) noexcept {
    static const BuiltinEffects kUnknown; // all-false: conservatively impure
    const auto& t = table();
    auto it = t.find(name);
    return (it != t.end()) ? it->second : kUnknown;
}

// ─────────────────────────────────────────────────────────────────────────────
// EGraphSubsystem — e-graph equality-saturation optimization subsystem
// ─────────────────────────────────────────────────────────────────────────────
//
// Delegates to the egraph:: free functions (egraph_optimizer.cpp) but wraps
// them with per-run configuration injection and statistics accumulation.
// All e-graph access from the Orchestrator and CodeGenerator goes through
// this class rather than calling egraph::optimizeProgram() directly.

std::unique_ptr<Expression> EGraphSubsystem::optimizeExpression(const Expression* expr) {
    if (!expr) return nullptr;

    ++stats_.expressionsAttempted;

    // Build context from our configuration and registered pure user functions.
    const egraph::EGraphOptContext ctx = toOptContext();
    auto result = egraph::optimizeExpression(expr, ctx);

    if (!result) {
        ++stats_.expressionsSkipped;
    } else {
        ++stats_.expressionsSimplified;
    }
    return result;
}

void EGraphSubsystem::optimizeFunction(FunctionDecl* func) {
    if (!func || !func->body) return;

    const unsigned before = stats_.expressionsSimplified;
    const egraph::EGraphOptContext ctx = toOptContext();
    egraph::optimizeFunction(func, ctx);
    if (stats_.expressionsSimplified > before) {
        ++stats_.functionsChanged;
    }
}

void EGraphSubsystem::optimizeProgram(Program* program) {
    if (!program) return;
    stats_.reset();

    const egraph::EGraphOptContext ctx = toOptContext();
    for (auto& func : program->functions) {
        const unsigned before = stats_.expressionsSimplified;
        egraph::optimizeFunction(func.get(), ctx);
        if (stats_.expressionsSimplified > before) {
            ++stats_.functionsChanged;
        }
    }
}

} // namespace omscript
