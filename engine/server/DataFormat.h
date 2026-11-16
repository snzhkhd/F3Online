#pragma once
// Canonical text form of the data/*.json files: one emitter shared by the exporter (--export-data) and the content editor
// (docs/editor.md), so a file saved by either produces the same bytes and git diffs stay clean.
//   objects: one field per line (two-space indent); small flat objects below the record level stay on one line;
//   arrays of scalars on one line; arrays of objects one element per block; floats with an integral value without ".0"; LF endings.
#include <ostream>
#include <string>
#include <nlohmann/json.hpp>

namespace f3 {

struct Proto;

void EmitJson(std::ostream& o, const nlohmann::ordered_json& j, int indent = 0);
std::string EmitJsonText(const nlohmann::ordered_json& j);   // EmitJson + trailing "\n"

// Records of the original (base) prototype in the exporter's field layout (Export.cpp); used as the base value of the editor's diff view.
nlohmann::ordered_json ExportItemRecord(const Proto& p);
nlohmann::ordered_json ExportNpcRecord(const Proto& p);   // template defaults only (no map instances)

}  // namespace f3
