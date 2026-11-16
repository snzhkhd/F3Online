// Machine-readable schema of data/ for the content editor (GET /api/schema, docs/editor.md). The form editor, the docs and the validator must not drift:
//  * everything the validator enumerates itself (skill names, SPECIAL keys, stat names, dialog stats, envelope version) is taken from the same
//    constants at run time, never retyped here;
//  * the numeric limits / string lengths below mirror Content.cpp; f3_editortest walks this schema and feeds boundary values to the real
//    validator (min and max accepted, min-1 / max+1 / unknown fields / over-long strings rejected), so a changed limit fails the test.
#include "Content.h"
#include "EditorCore.h"
#include "common/protocol_game.h"

namespace f3::editor {

namespace {

// Field types: string text int number bool enum intenum object list stringlist intmap cell condition effects statmods dlgstart
// Keys: key label type required help min max maxLen enum labels ref fields item keys idPattern luaName fixedLen maxCount section appliesTo default
const char* const kSkills = R"J({
 "id":"skills","label":"Skills","group":"Content","dir":"skills","nameField":"id","idEnum":true,
 "help":"The 14 skills are fixed; a record overrides the base formula (const + sum of factor x SPECIAL) and the description. `original` is the original game's formula, reference only.",
 "fields":[
  {"key":"id","type":"enum","enum":"$skills","required":true,"label":"Skill"},
  {"key":"desc","type":"text","maxLen":600,"label":"Description"},
  {"key":"base","type":"object","label":"Base formula","help":"skill base = const + sum(factor * SPECIAL); replaces the whole formula when present","fields":[
    {"key":"const","type":"int","min":-1000,"max":1000},
    {"$special":{"type":"int","min":-100,"max":100}}]},
  {"key":"original","type":"object","label":"Original game formula (reference only, never applied)","fields":[
    {"key":"const","type":"int","min":-1000,"max":1000},
    {"$special":{"type":"int","min":-100,"max":100}}]}
 ]})J";

const char* const kTraits = R"J({
 "id":"traits","label":"Traits","group":"Content","dir":"traits","nameField":"name",
 "envelope":[{"key":"max_picks","type":"int","min":0,"max":16,"label":"max_picks (traits allowed at creation, 0..16; two files must not disagree)"}],
 "help":"The trait list of the creation screen. `index` is the bit of the character's trait mask: never renumber a shipped trait.",
 "fields":[
  {"key":"id","type":"string","required":true,"maxLen":64,"idPattern":true,"label":"Id"},
  {"key":"name","type":"string","required":true,"maxLen":64,"label":"Name"},
  {"key":"desc","type":"text","maxLen":800,"label":"Description"},
  {"key":"index","type":"int","required":true,"min":0,"max":15,"label":"Index (creation slot / mask bit)"},
  {"key":"hook","type":"string","maxLen":64,"luaName":true,"label":"Lua hook"},
  {"key":"effects","type":"statmods","label":"Effects"}
 ]})J";

const char* const kPerks = R"J({
 "id":"perks","label":"Perks","group":"Content","dir":"perks","nameField":"name",
 "help":"Perk definitions: requirements, declarative stat effects (applied once per rank) and an optional Lua hook.",
 "fields":[
  {"key":"id","type":"string","required":true,"maxLen":64,"idPattern":true,"label":"Id"},
  {"key":"name","type":"string","required":true,"maxLen":64,"label":"Name"},
  {"key":"desc","type":"text","maxLen":800,"label":"Description"},
  {"key":"ranks","type":"int","min":1,"max":10,"default":1,"label":"Ranks"},
  {"key":"hook","type":"string","maxLen":64,"luaName":true,"label":"Lua hook (called as fn(critter, perk_id, rank))"},
  {"key":"placeholder","type":"bool","label":"Placeholder numbers (not read from the original game)"},
  {"key":"requires","type":"object","label":"Requirements","fields":[
    {"key":"level","type":"int","min":1,"max":99,"label":"Level"},
    {"key":"special","type":"object","label":"Minimum SPECIAL","fields":[{"$special":{"type":"int","min":1,"max":10}}]},
    {"key":"skills","type":"intmap","keys":"$skills","min":1,"max":300,"label":"Minimum skills"},
    {"key":"perks","type":"stringlist","ref":"perk","maxCount":20,"label":"Perks that must be held"},
    {"key":"traits","type":"stringlist","ref":"trait","maxCount":20,"label":"Traits that must be chosen"}]},
  {"key":"effects","type":"statmods","label":"Effects (per rank)"}
 ]})J";

const char* const kItems = R"J({
 "id":"items","label":"Items","group":"Content","dir":"items","nameField":"name","hasBase":true,"idRef":"proto",
 "help":"Overrides of original prototypes (id = an existing prototype; only the written fields change) or new items (`base` + a new id).",
 "fields":[
  {"key":"id","type":"string","required":true,"maxLen":128,"label":"Id (prototype key)"},
  {"key":"base","type":"string","maxLen":128,"ref":"item","label":"Base (clone this original prototype; for a new id)"},
  {"key":"name","type":"string","maxLen":120,"label":"Name"},
  {"key":"desc","type":"text","maxLen":1200,"label":"Description"},
  {"key":"weight","type":"int","min":0,"max":10000,"label":"Weight"},
  {"key":"model","type":"string","maxLen":128,"label":"Model (informational)"},
  {"key":"held_model","type":"string","maxLen":128,"label":"Held / worn model (informational)"},
  {"key":"head_armor","type":"bool","appliesTo":"armor","label":"Worn on the head"},
  {"key":"weapon","type":"object","appliesTo":"weapon","label":"Weapon","fields":[
    {"key":"range","type":"int","min":1,"max":200},
    {"key":"dmg_min","type":"number","min":0,"max":10000},
    {"key":"dmg_max","type":"number","min":0,"max":10000},
    {"key":"caliber","type":"int","min":-1,"max":200},
    {"key":"modes","type":"list","maxCount":8,"label":"Attack modes (replace the whole list)","item":{"type":"object","fields":[
      {"key":"type","type":"int","min":0,"max":99,"required":true},
      {"key":"cost","type":"int","min":0,"max":10000,"required":true},
      {"key":"dmg_min","type":"int","min":0,"max":10000},
      {"key":"dmg_max","type":"int","min":0,"max":10000},
      {"key":"ap","type":"int","min":0,"max":100,"label":"ap (real action points, reference)"},
      {"key":"shots","type":"int","min":0,"max":50},
      {"key":"damage_type","type":"intenum","min":0,"max":5,"labels":"$damageTypes"}]}}]},
  {"key":"armor","type":"object","appliesTo":"armor","label":"Armor","fields":[
    {"key":"values","type":"list","fixedLen":7,"label":"The 7 armor words","item":{"type":"int","min":0,"max":1000}}]},
  {"key":"ammo","type":"object","appliesTo":"ammo","label":"Ammunition","fields":[
    {"key":"dmg_min","type":"int","min":0,"max":10000},
    {"key":"dmg_max","type":"int","min":0,"max":10000},
    {"key":"caliber","type":"int","min":0,"max":200},
    {"key":"armor_adj","type":"int","min":-1000,"max":1000},
    {"key":"accuracy_adj","type":"int","min":-1000,"max":1000}]}
 ]})J";

const char* const kNpcs = R"J({
 "id":"npcs","label":"NPCs","group":"Content","dir":"npcs","nameField":"name","hasBase":true,"idRef":"proto",
 "help":"Overrides of original critter prototypes or new NPCs (`base` + a new id), with optional spawn points.",
 "fields":[
  {"key":"id","type":"string","required":true,"maxLen":128,"label":"Id (prototype key)"},
  {"key":"base","type":"string","maxLen":128,"ref":"npc","label":"Base (clone this original critter; for a new id)"},
  {"key":"name","type":"string","maxLen":120,"label":"Name"},
  {"key":"model","type":"string","maxLen":128,"label":"Model (informational)"},
  {"key":"team","type":"int","min":0,"max":255},
  {"key":"level","type":"int","min":1,"max":99},
  {"key":"max_hp","type":"int","min":1,"max":9999},
  {"key":"xp","type":"int","min":0,"max":1000000,"label":"XP for killing it"},
  {"key":"respawn","type":"int","min":0,"max":604800,"label":"Respawn after death, seconds (0 = never; empty = server default RespawnDefaultSec)"},
  {"key":"special","type":"object","label":"SPECIAL (omitted values keep the original)","fields":[{"$special":{"type":"int","min":1,"max":10}}]},
  {"key":"script","type":"string","maxLen":128,"label":"AI script"},
  {"key":"dialog","type":"string","maxLen":128,"ref":"dialog","label":"Dialog (a data dialog id, or an original dialog name)"},
  {"key":"equipment","type":"stringlist","ref":"item","maxCount":50,"label":"Equipment (worn on spawn)"},
  {"key":"inventory","type":"stringlist","ref":"item","maxCount":200,"label":"Inventory (carried / loot)"},
  {"key":"spawns","type":"list","label":"Spawn points (run once at startup)","item":{"type":"object","fields":[
    {"key":"map","type":"string","required":true,"maxLen":128,"ref":"map"},
    {"key":"cell","type":"cell","required":true,"label":"Cell [x, y]"},
    {"key":"count","type":"int","min":1,"max":50}]}}
 ]})J";

const char* const kQuests = R"J({
 "id":"quests","label":"Quests","group":"Content","dir":"quests","nameField":"title",
 "help":"Quest definitions for the Pip-Boy log; rewards are paid once per character by quest_done / a dialog effect.",
 "fields":[
  {"key":"id","type":"string","required":true,"maxLen":48,"idPattern":true,"label":"Id"},
  {"key":"title","type":"string","required":true,"maxLen":80,"label":"Title"},
  {"key":"text","type":"text","maxLen":600,"label":"Text"},
  {"key":"xp","type":"int","min":0,"max":1000000,"label":"XP reward"},
  {"key":"items","type":"list","label":"Item rewards","item":{"type":"object","fields":[
    {"key":"item","type":"string","required":true,"maxLen":128,"ref":"item"},
    {"key":"count","type":"int","min":1,"max":999}]}}
 ]})J";

const char* const kDialogs = R"J({
 "id":"dialogs","label":"Dialogs","group":"Content","dir":"dialogs","nameField":"id","editor":"dialog",
 "help":"Conversation graphs run by the server (docs/dialogs.md). Use the Graph tab for nodes, answers, conditions and effects.",
 "fields":[
  {"key":"id","type":"string","required":true,"maxLen":64,"idPattern":true,"label":"Id"},
  {"key":"speaker","type":"string","maxLen":128,"ref":"npc","label":"Speaker (critter prototype)"},
  {"key":"start","type":"dlgstart","label":"Start (node id, or routes { if, goto })"},
  {"key":"nodes","type":"list","required":true,"minCount":1,"maxCount":200,"label":"Nodes","item":{"type":"object","textOrStf":true,"fields":[
    {"key":"id","type":"string","required":true,"maxLen":48,"nameChars":true},
    {"key":"text","type":"text","maxLen":1500},
    {"key":"stf","type":"int","min":1,"max":1000000,"ref":"stf","label":"stf (original English.stf string id)"},
    {"key":"on_enter","type":"effects","maxCount":16},
    {"key":"answers","type":"list","maxCount":24,"item":{"type":"object","textOrStf":true,"fields":[
      {"key":"id","type":"string","maxLen":48,"nameChars":true},
      {"key":"text","type":"text","maxLen":1500},
      {"key":"stf","type":"int","min":1,"max":1000000,"ref":"stf"},
      {"key":"if","type":"condition"},
      {"key":"do","type":"effects","maxCount":16},
      {"key":"goto","type":"string","maxLen":48,"ref":"node"}]}}]}}
 ]})J";

const char* const kObjects = R"J({
 "id":"objects","label":"Object scripts","group":"Content","dir":"objects","nameField":"id","editor":"objects",
 "help":"Behaviour bound to map objects (computers, terminals, levers, doors, containers), docs/object-scripts.md. Heavy logic: a Lua file in scripts/objects/; simple behaviour: the declarative steps below.",
 "fields":[
  {"key":"id","type":"string","required":true,"maxLen":64,"idPattern":true,"label":"Id"},
  {"key":"map","type":"string","required":true,"maxLen":128,"ref":"map","label":"Map"},
  {"key":"object","type":"string","maxLen":128,"label":"Object (instance name on the map; the stable binding key)"},
  {"key":"proto","type":"string","maxLen":128,"label":"Prototype (fallback binding, with cell)"},
  {"key":"cell","type":"cell","label":"Cell [x, y] (fallback binding)"},
  {"key":"label","type":"string","maxLen":32,"label":"Cursor label ([Use] by default)"},
  {"key":"name","type":"string","maxLen":128,"label":"Name override"},
  {"key":"desc","type":"text","maxLen":600,"label":"Description override"},
  {"key":"script","type":"scriptpath","maxLen":128,"label":"Lua script (below scripts/)"},
  {"key":"usable","type":"bool","default":true,"label":"Usable"},
  {"key":"one_shot","type":"oneshot","label":"One shot (true / world / player)"},
  {"key":"cooldown","type":"int","min":0,"max":86400,"label":"Cooldown (seconds)"},
  {"key":"range","type":"int","min":0,"max":50,"label":"Range (tiles, 0 = door range)"},
  {"key":"hidden_until","type":"condition","label":"Hidden until"},
  {"key":"on_use","type":"steps","maxCount":24,"label":"On use"},
  {"key":"on_look","type":"steps","maxCount":24,"label":"On look"},
  {"key":"skills","type":"stepmap","keys":"$skills","label":"On skill use"}
 ]})J";

const char* const kMisc = R"J({
 "conditions":{
  "ops":[">=","<=","==","!=",">","<"],
  "combinators":["all","any","not"],
  "maxItems":16,
  "kinds":[
   {"id":"skill","name":{"type":"enum","enum":"$skills"},"op":"required","value":{"type":"int","min":0,"max":300}},
   {"id":"stat","name":{"type":"enum","enum":"$dlgStats"},"op":"required","value":{"type":"int","min":0,"max":1000000}},
   {"id":"item","name":{"type":"string","ref":"item"},"op":"optional","value":{"type":"int","min":0,"max":100000}},
   {"id":"quest","name":{"type":"string","ref":"quest"},"op":"eq-ne","value":{"type":"enum","enum":"$questStates"}},
   {"id":"flag","name":{"type":"string"},"op":"optional","value":{"type":"int","min":-1000000,"max":1000000}},
   {"id":"lua","name":{"type":"string","luaName":true},"op":"none"}]},
 "effects":{
  "maxItems":16,
  "kinds":[
   {"id":"give","fields":[{"key":"give","type":"string","ref":"item","required":true},{"key":"count","type":"int","min":1,"max":9999,"default":1}]},
   {"id":"take","fields":[{"key":"take","type":"string","ref":"item","required":true},{"key":"count","type":"int","min":1,"max":9999,"default":1}]},
   {"id":"quest","fields":[{"key":"quest","type":"string","ref":"quest","required":true},{"key":"state","type":"enum","enum":"$questActions","required":true}]},
   {"id":"flag","fields":[{"key":"flag","type":"string","required":true},{"key":"set","type":"flagvalue","default":true},{"key":"world","type":"bool","label":"shared by everyone"}]},
   {"id":"xp","fields":[{"key":"xp","type":"int","min":1,"max":100000,"required":true}]},
   {"id":"trade","fields":[{"key":"trade","type":"const","value":true}],"help":"stub: there is no store yet"},
   {"id":"attack","fields":[{"key":"attack","type":"const","value":true}],"help":"ends the dialog; the NPC turns hostile"},
   {"id":"end","fields":[{"key":"end","type":"const","value":true}],"help":"ends the dialog; later effects are skipped"},
   {"id":"door","fields":[{"key":"door","type":"cell","required":true},{"key":"action","type":"enum","enum":"$doorActions","required":true}]},
   {"id":"lua","fields":[{"key":"lua","type":"string","luaName":true,"required":true}]}]},
 "steps":{
  "maxItems":24,
  "doorActions":["open","close","toggle","lock","unlock"],
  "kinds":[
   {"id":"dialog","fields":[{"key":"dialog","type":"string","ref":"dialog","required":true}],"help":"starts a data dialog spoken by the object; the rest of the list is not run"},
   {"id":"door","fields":[{"key":"door","type":"doortarget","required":true},{"key":"action","type":"enum","enum":["open","close","toggle","lock","unlock"],"required":true}],"help":"a door / container of the same map: its instance name or cell [x, y]"},
   {"id":"skill","fields":[{"key":"skill","type":"enum","enum":"$skills","required":true},{"key":"difficulty","type":"int","min":-200,"max":400,"default":0},{"key":"roll","type":"bool","label":"roll a percent chance"},{"key":"reward_xp","type":"int","min":0,"max":100000,"label":"xp on success"},{"key":"success","type":"steps"},{"key":"failure","type":"steps"}]},
   {"id":"give","fields":[{"key":"give","type":"string","ref":"item","required":true},{"key":"count","type":"int","min":1,"max":9999,"default":1}]},
   {"id":"take","fields":[{"key":"take","type":"string","ref":"item","required":true},{"key":"count","type":"int","min":1,"max":9999,"default":1}]},
   {"id":"quest","fields":[{"key":"quest","type":"string","ref":"quest","required":true},{"key":"state","type":"enum","enum":"$questActions","required":true}]},
   {"id":"flag","fields":[{"key":"flag","type":"string","required":true},{"key":"set","type":"flagvalue","default":true},{"key":"world","type":"bool","label":"shared by everyone"}]},
   {"id":"xp","fields":[{"key":"xp","type":"int","min":1,"max":100000,"required":true}]},
   {"id":"message","fields":[{"key":"message","type":"text","maxLen":600,"required":true},{"key":"key","type":"string","maxLen":96,"label":"locale key (optional)"}]},
   {"id":"teleport","fields":[{"key":"teleport","type":"string","ref":"map","required":true},{"key":"entry","type":"int","min":0,"max":255},{"key":"cell","type":"cell"}],"help":"map transition: entry point or cell"},
   {"id":"play","fields":[{"key":"play","type":"string","maxLen":64,"required":true}],"help":"calls the Lua hook object_effect(player, obj, name); the protocol has no effect message yet"},
   {"id":"call","fields":[{"key":"call","type":"string","luaName":true,"required":true}],"help":"global Lua function fn(player, obj)"},
   {"id":"default_use","fields":[{"key":"default_use","type":"const","value":true}],"help":"the engine's own door / container use"},
   {"id":"original","fields":[{"key":"original","type":"const","value":true}],"help":"runs the object's original script (scripts/original, e.g. the Vault 13 computer) at this point: compose it with your own steps"},
   {"id":"end","fields":[{"key":"end","type":"const","value":true}],"help":"stops the list"}]},
 "extensionPoints":{}
})J";

// "$name" strings in the schema are replaced by the live lists below.
json Expand(json j, const std::map<std::string, json>& enums) {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s.size() > 1 && s[0] == '$') { auto it = enums.find(s.substr(1)); if (it != enums.end()) return it->second; }
        return j;
    }
    if (j.is_array()) {
        json out = json::array();
        for (const json& e : j) {
            if (e.is_object() && e.contains("$special")) {   // one field per SPECIAL key
                for (const std::string& k : SpecialKeyNames()) { json f = Expand(e["$special"], enums); f["key"] = k; out.push_back(f); }
            } else out.push_back(Expand(e, enums));
        }
        return out;
    }
    if (j.is_object()) {
        json out = json::object();
        for (auto it = j.begin(); it != j.end(); ++it) out[it.key()] = Expand(it.value(), enums);
        return out;
    }
    return j;
}

}  // namespace

const std::vector<KindInfo>& Kinds() {
    static const std::vector<KindInfo> k = {
        {"items", "Items", "Content"}, {"npcs", "NPCs", "Content"}, {"perks", "Perks", "Content"}, {"traits", "Traits", "Content"},
        {"skills", "Skills", "Content"}, {"quests", "Quests", "Content"}, {"dialogs", "Dialogs", "Content"}, {"objects", "Object scripts", "Content"}};
    return k;
}

json BuildSchema() {
    std::map<std::string, json> enums;
    json skills = json::array();
    for (int i = 0; i < kSkillCount; ++i) skills.push_back(kSkillNames[i]);
    enums["skills"] = skills;
    enums["special"] = SpecialKeyNames();
    enums["stats"] = StatNames();
    enums["dlgStats"] = DialogStatNames();
    enums["damageTypes"] = json::array({"ballistic", "biological", "electrical", "EMP", "normal", "heat"});
    enums["questStates"] = json::array({"none", "active", "done", "failed"});
    enums["questActions"] = json::array({"add", "done", "fail", "reopen"});
    enums["doorActions"] = json::array({"open", "close", "lock", "unlock"});
    json out = json::object();
    out["version"] = 1;
    out["contentSchema"] = kContentSchema;
    out["enums"] = enums;
    json kinds = json::object();
    for (const char* src : {kItems, kNpcs, kPerks, kTraits, kSkills, kQuests, kDialogs, kObjects}) {
        json k = json::parse(src);
        const std::string id = k["id"].get<std::string>();
        kinds[id] = Expand(k, enums);
    }
    out["kinds"] = kinds;
    json order = json::array();
    for (const KindInfo& ki : Kinds()) order.push_back(ki.id);
    out["kindOrder"] = order;
    json misc = Expand(json::parse(kMisc), enums);
    out["conditions"] = misc["conditions"];
    out["effects"] = misc["effects"];
    out["steps"] = misc["steps"];
    out["extensionPoints"] = misc["extensionPoints"];
    out["statEffect"] = json{{"stat", {{"type", "enum"}, {"enum", enums["stats"]}}}, {"add", {{"type", "int"}, {"min", -1000}, {"max", 1000}}}};
    return out;
}

}  // namespace f3::editor
