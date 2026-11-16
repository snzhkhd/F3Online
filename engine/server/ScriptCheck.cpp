// Syntax / hook check of an object script in a throw-away sandbox (editor "live validation", docs/object-scripts.md). Kept in its own file so the
// editor test can link it without the whole server.
#include "Scripting.h"
#include "common/lua_bind.h"

namespace f3 {

Scripting::ScriptCheck Scripting::CheckScript(const std::filesystem::path& file) {
    ScriptCheck c;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return c;
    c.exists = true;
    auto Lp = lua::NewSandboxState();
    sol::state_view L(*Lp);
    sol::load_result lr = L.load_file(file.string());
    if (!lr.valid()) { sol::error e = lr; c.error = e.what(); return c; }
    c.ok = true;
    sol::environment env(L, sol::create, L.globals());
    sol::protected_function fn = lr;
    sol::set_environment(env, fn);
    sol::protected_function_result r = fn();
    if (!r.valid()) { sol::error e = r; c.warning = std::string("top-level code failed in the check sandbox: ") + e.what(); }
    for (const char* h : {"on_use", "on_skill", "on_look", "can_use", "on_loaded"}) {
        sol::object f = env[h];
        if (f.get_type() == sol::type::function) c.hooks.push_back(h);
    }
    return c;
}

}  // namespace f3
