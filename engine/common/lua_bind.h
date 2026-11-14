#pragma once
// Small helpers around sol2: sandboxed state creation, safe file loading, safe hook calls.
#include <filesystem>
#include <memory>
#include <string>
#include <sol/sol.hpp>
#include "log.h"

namespace f3::lua {

// A state with only side-effect-free libraries: no io/os/debug/package, no dofile/loadfile/require.
inline std::unique_ptr<sol::state> NewSandboxState() {
    auto L = std::make_unique<sol::state>();
    L->open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math,
                      sol::lib::utf8, sol::lib::coroutine);
    (*L)["dofile"] = sol::nil;
    (*L)["loadfile"] = sol::nil;
    (*L)["require"] = sol::nil;
    return L;
}

// Runs a file; first return value (nil if none) goes to *result when non-null.
inline bool RunFile(sol::state_view L, const std::filesystem::path& path, sol::object* result = nullptr) {
    sol::protected_function_result r = L.safe_script_file(path.string(), sol::script_pass_on_error);
    if (!r.valid()) {
        sol::error e = r;
        log::Error("lua: %s: %s", path.string().c_str(), e.what());
        return false;
    }
    if (result) *result = (r.return_count() > 0) ? r.get<sol::object>() : sol::make_object(L, sol::nil);
    return true;
}

// Calls global function `name` if defined. Returns false when missing or when it raised an error.
// First return value goes to *ret if provided.
template <class... Args>
bool CallGlobal(sol::state_view L, const char* name, sol::object* ret, Args&&... args) {
    sol::object fn = L[name];
    if (fn.get_type() != sol::type::function) return false;
    sol::protected_function pf = fn.as<sol::protected_function>();
    sol::protected_function_result r = pf(std::forward<Args>(args)...);
    if (!r.valid()) {
        sol::error e = r;
        log::Error("lua: hook %s: %s", name, e.what());
        return false;
    }
    if (ret) *ret = (r.return_count() > 0) ? r.get<sol::object>() : sol::make_object(L, sol::nil);
    return true;
}

}  // namespace f3::lua
