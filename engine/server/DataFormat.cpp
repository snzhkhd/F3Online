#include "DataFormat.h"
#include <cmath>
#include <sstream>

namespace f3 {

using nlohmann::ordered_json;

namespace {
void EmitStr(std::ostream& o, const std::string& s) { o << ordered_json(s).dump(-1, ' ', false); }
}  // namespace

void EmitJson(std::ostream& o, const ordered_json& j, int ind) {
    const std::string pad(static_cast<size_t>(ind) * 2, ' '), pad1(static_cast<size_t>(ind + 1) * 2, ' ');
    if (j.is_object()) {
        if (j.empty()) { o << "{}"; return; }
        bool simple = true;   // small flat objects below the record level (a skill formula, a stat block, an attack mode) stay on one line
        for (auto& [k, v] : j.items()) {
            if (v.is_object()) simple = false;
            if (v.is_array()) for (auto& e : v) if (e.is_object() || e.is_array()) simple = false;
        }
        if (simple && ind >= 3 && j.dump().size() < 130) {
            o << "{ ";
            bool first = true;
            for (auto& [k, v] : j.items()) { if (!first) o << ", "; first = false; EmitStr(o, k); o << ": "; EmitJson(o, v, ind + 1); }
            o << " }";
            return;
        }
        o << "{\n";
        bool first = true;
        for (auto& [k, v] : j.items()) {
            if (!first) o << ",\n";
            first = false;
            o << pad1;
            EmitStr(o, k);
            o << ": ";
            EmitJson(o, v, ind + 1);
        }
        o << "\n" << pad << "}";
    } else if (j.is_array()) {
        if (j.empty()) { o << "[]"; return; }
        bool scalar = true;
        for (auto& v : j) if (v.is_object() || v.is_array()) scalar = false;
        if (scalar) {
            o << "[";
            for (size_t i = 0; i < j.size(); ++i) { if (i) o << ", "; EmitJson(o, j[i], ind + 1); }
            o << "]";
            return;
        }
        o << "[\n";
        for (size_t i = 0; i < j.size(); ++i) {
            o << pad1;
            EmitJson(o, j[i], ind + 1);
            o << (i + 1 < j.size() ? ",\n" : "\n");
        }
        o << pad << "]";
    } else if (j.is_number_float()) {
        const double d = j.get<double>();
        if (std::floor(d) == d && std::fabs(d) < 1e9) o << static_cast<long long>(d);
        else o << j.dump();
    } else if (j.is_string()) {
        EmitStr(o, j.get<std::string>());
    } else {
        o << j.dump();
    }
}

std::string EmitJsonText(const ordered_json& j) {
    std::ostringstream o;
    EmitJson(o, j, 0);
    o << "\n";
    return o.str();
}

}  // namespace f3
