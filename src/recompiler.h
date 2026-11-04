#pragma once
#include "decoder.h"
#include <map>

namespace smbd {
struct LoweringIssue {
    std::uint32_t address;
    std::string bytes, instruction, reason;
};
struct LoweredFunction {
    std::string source;
    std::vector<LoweringIssue> issues;
    std::map<std::string,std::string> native_helpers;
    bool floating = false;
    std::string native_api;
    // Original instructions whose generated code changes time semantics.
    // Export the actual lowering, so old fixes remain part of the audit.
    std::vector<std::pair<std::uint32_t,std::string>> timing_sites;
    std::string timing_entry;
};
LoweredFunction lower(const Function& function);
std::string recompile(Xbe& image, std::uint32_t address, std::uint32_t instruction_budget);
}
