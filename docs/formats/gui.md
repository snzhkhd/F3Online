# Interface layouts (`Interface\*.gui`, resource type 1600) and skins (`*.ini`, type 800)

Status: **the .gui layout format is fully decoded** (all 25 files of the demo parse to the end of the file with no trailing
bytes, every widget type and every field the engine's own reader consumes is read). The record layout was taken from the
engine's reader in the decompilation (`vbdecomp-main`, `openje/src/gui/*Resource*.cpp`: `Interface::LoadResource`,
`Window/Label/Button/Fragment::ReadResource`) and verified against all files. Readers:

* `engine/common/gamedata/GuiFile.{h,cpp}`: `ParseGuiTree` (exact tree, all properties; use this for new windows) and the
  older heuristic `ParseGui` (flat widget list, still used by `OrigUi` for the action bar and the inventory).
* `f3_gdtool <gamedir> guilist` (every file: version, node counts per type), `f3_gdtool <gamedir> guitree <Name>`
  (full dump of the tree) and `f3_gdtool <gamedir> png <TgaName> <out.png>` (export an interface texture).
* `engine/common/gamedata/Stf.{h,cpp}`: `English.stf`, the string table the layouts reference by id.

## .gui layout file (version 4 = magic `GUI V1.3`)

Little endian. Strings are `u32 length` + bytes (no terminator). The magic is `"GUI "` + `"V1.0".."V1.3"` (file version
1..4); an older file without magic is version 0. The demo has v2 (Loading), v3 (Debug, DebugTools, AreaMap, Dialog,
Feedback) and v4 (everything else); the reader handles the differences by version.

```
"GUI " "V1.3"           8 bytes magic
string  name            "CharacterGeneration.INT_new_WIP.INT"
u8      hidden
u32     root count
root count x { u32 type; widget record }

widget types: 0 Window, 1 Button, 2 Picture, 3 Label, 4 Edit
widget record (recursive):
  [Button only]  u32 state;  if version >= 4: u8 keyboard, string soundDown, string soundUp, string soundEnter
  i32[4] rect               left, top, right, bottom -- ABSOLUTE layout coordinates (root space), not parent relative
  string name
  u8 hidden, u8 frameVisible, u8 centerVisible
  string skin               skin ini ("zzmainmenu.ini", "quickslot.ini", "DefaultInterface.ini", ...)
  u32 color                 tint, bytes B G R A (A = alpha; 128,128,128 = neutral)
  i32[4] clientRect         second rectangle, RELATIVE to the parent (the old heuristic reader exposes this one)
  if version >= 2: u8 value60, u32 materialModes
  if version >= 3: string str0, u32 tipTextRef, string str1, u32 tipTitleRef, string tipImage
                           tooltip: tipTitleRef = English.stf id of the heading, tipTextRef = id of the description
                           (0xFFFFFFFF = none), tipImage = picture shown beside it (f3_tooltip_strength.tga ...)
                           NOTE: gdtool prints the pair as "tip=<title>/<text>"
  9 x fragment { u32 tileMode, frameW, frameH, frameRate, curFrame, frameCount; string texture; f32 z; u32 color; i32[4] rect }
                           the nine skin pieces UL U UR L M R LL LO LR; the middle one (index 4) is the picture
  { u32 index; string }*   indexed per-widget strings, terminated by index 0xFFFFFFFF
  u32 child count; child count x { u32 type; widget record }
  [Label and Edit only, AFTER the children]
    u32 strRef            English.stf id of the text (7 = "None" is a placeholder for texts the game fills in)
    string fontName       "fallouty" / "Gothic 821 Condensed BT" / "Arial"
    u32 fontFlags, fontSize, fontCharset, textColor (ARGB, e.g. FF008000 = green)
    u8 textFrame, u8 wrap, u32 alignment (0 left, 1 centre, 2 right), u32 padding[4]
    u32 valign            (version >= 1)
```

* Buttons are `Button` nodes whose pictures are children by naming convention: `BtnUp`, `BtnDown` (hidden), `BtnDisabled`
  (hidden), sometimes with a suffix (`BtnUp_Done`). The engine shows one of them depending on the state; the file does not say
  which, the convention is in the code. `OrigUiChar.cpp` follows it.
* Skin textures come from the skin ini; for pictures and buttons the *middle* fragment is the artwork.
* Textures are authored at 800x600-ish and the layouts are 1024x768-ish: `picBackground` of the creation screen is 1024x768 but
  its texture `f3_chargen.tga` is 800x600 (scale 1.28); the digit pictures (`f3_digit_N.tga` 16x24) fill 20x30 windows.
* Texts are English.stf ids (`strRef`, `tipTitleRef`, `tipTextRef`); `English.stf` (v3: u32 version, u32 ?, u32 count, then
  `count` 16-byte records {offset, length, ...}, Latin-1 text, ids are 1-based) sits in the game root.

### What is still unknown

* The meaning of `value60`, `materialModes`, `str0` / `str1` (always empty in the demo), the indexed strings (read, content not
  interpreted), `fragment.z`, `frameRate / curFrame / frameCount` (animated fragments: the demo layouts do not seem to use
  them) and the tile modes beyond "0 fixed / 1 stretch horizontally / 2 stretch vertically / 3 stretch both" (skin ini).
* `fontFlags`, `fontCharset`, `textFrame`, `wrap`, `valign` values (all demo labels use the same few values; handled as
  left / centre / right, no wrap).
* Behaviour is not data: which child picture a button shows in which state, what a window does on click, hover sounds, the
  tab switching, validation, etc. live in the engine (`F3.exe`), not in the layouts. The client implements these per window.
* The skin ini files are only partly interpreted (nine-piece windows are not drawn from them; the demo layouts use per-widget
  textures instead).

## All layouts of the demo and their status in this client

"OrigUi" = drawn from the original layout and art by `engine/client/OrigUi*.cpp`. "raylib" = a plain raylib window of ours
(`RaylibRenderer.cpp`), to be replaced. "-" = not implemented.

| file | version, widgets | purpose in the original | status |
|---|---|---|---|
| Actionbar | v4, 94 | bottom bar: weapon slots, AP / HP / evasion digits, party frames, message log, Inventory / Character / Options buttons | **OrigUi** (action bar, chat in the message area, hands, AP) |
| Inventory | v4, 167 | backpack, paperdoll, per-slot stats, description | **OrigUi** (list, equip slots, drag and drop, sort, examine) |
| CharacterGeneration | v4, 564 | full-screen character creation (SPECIAL, 13 skills + tags, 16 traits, derived stats, thresholds, appearance) and the same screen as the character window | **OrigUi**: creation screen and the C window (`OrigUiChar.cpp`). Not built: the Appearance row (no appearance data on the server), the character model, the Options button, Age / Sex plates |
| CharGen_NAS | v4, 24 | name / age / sex sub-dialog of the creation | - (the server has no age / sex) |
| Container | v4, 16 | container window (256x176: ten 50x50 slots Slot0..9 in 5x2, TakeAll, Done, Name; English.stf 1713 / 75) | **OrigUi** (`DrawLoot`: layout geometry and labels; the window skin is not drawn, a dark panel stands in; items of the target here, the player's side is the inventory window beside it; drag / double click, Take All, Done) |
| Dialog | v3, 9 | speaker / listener model windows of the dialogue (the text window is drawn by the engine) | raylib (dialogue box) |
| Mainmenu | v4, 9 | main menu: New Game, Load, Multi, Cinematics, Options, Quit | - (a raylib login screen replaces it) |
| EscapeMenu | v4, 22 | Esc menu: Continue, Load/Save, Options, Quit | - |
| MessageBox | v4, 21 | Yes / No / Done confirmation | - |
| PipBoy | v4, 187 | Pip-Boy: time, maps, notes, holodisks | - |
| AreaMap, AreaMapNote, AddNote | v3 / v4, 75 / 15 / 8 | area (town) map with markers and notes | - |
| MiniMap | v4, 1 | minimap frame | - |
| Loading | v2, 3 | loading screen | - |
| Feedback | v3, 2 | feedback / bug report window | - |
| Debug, DebugTools | v3, 2 / 19 | developer windows (resume, one round, break points, set entity, scripts) | - |
| EditToken | v4, 7 | edit a token (developer) | - |
| Options, Options_Game, Options_Video, Options_Multiplayer, Options_Hotkeys, Options_Load | v4, 17 / 141 / 144 / 2 / 18 / 29 | options tabs (game, video, multiplayer, hotkeys), load / save | - |

The demo has no separate in-game character screen: the original reuses `CharacterGeneration` for it, and so does this client
(`OrigUi::DrawCharScreen(creating = false)`).

## Character creation / character window (`CharacterGeneration.gui`, layout 1024x768, background `f3_chargen.tga`)

All rectangles are absolute. Hover texts (heading, description, picture) go to the parchment: `lblToolTipTitle` (444,417),
`lblToolTip` (448,463), `picToolTip` (650,463,784,597).

| group | widgets | notes |
|---|---|---|
| SPECIAL | `winST/PE/EN/CH/IN/AG/LK` + `...10` / `...1` (digit windows 20x30), `lblXX` (rating word, English.stf 41..50 for 1..10), `btnXX+` / `btnXX-` (22x14) | the letters ST PE ... are part of the background |
| char points | `winCharPoints10/1` | points left to spend |
| skills | `btnSkillN` (radio, child BtnUp / BtnDown), `lblSkillNa` name, `lblSkillNb` base, `lblSkillNc` total, headings `lblSkillsHeading1..4` (stf 79..82), `lblSkillsHeading1a/1b` (Base 97, Total 98) | the layout has 13 rows (stf 83..95), the server has 14 skills (Traps, stf 96): one extra row is placed under Steal |
| tags | `winTagSkills10/1` | tags left; a skill is tagged by its radio |
| traits | `btnTraitNM` / `lblTraitNM` (N = column 1 or 2, M = row 1..8), stf 159..174 (descriptions 176..191) | `traitMax` (2) at most |
| stats tab | `lblDerivedStatNa/b` (1..14: AP, carry weight, sequence, critical chance, evasion, healing rate, melee damage, perception range, perk rate, skill rate, throwing range, then Biological / Flash / Radiation), `picTabStats` | values the server does not model show "-" |
| thresholds tab | `winThresholds` children `lblHead*`, `lblBody*` (Ballistic, Electrical, EMP, Heat, Biological) | `btnStats` / `btnThresholds` switch the tab |
| name plates | `btnName`, `btnAge`, `btnSex` (art only) | the client shows the account name on the name plate |
| appearance | `winAppearance`, 7 columns of options | not built |
| buttons | `winOptions` / `winDone` / `winCancel` (+ `btnX` with BtnUp / BtnDown = the red LED) | Done = create (creation) / close (window), Cancel = quit (creation) / close |
| hit points | `lblHP1` / `lblHP2` | in the black field above Options |

## Skin .ini (`zzmainmenu.ini`, `quickslot.ini`, `f3_green.ini`, ...)

```
[window.upperleft]   [window.uppermiddle] ... [window.lowerright]    nine fragments of a window
[button.up.*] [button.down.*] [button.disabled.*] [button.selected.*] ...
flags   = 0 fixed corner, 1 stretch horizontally, 2 stretch vertically, 3 stretch both
color   = r,g,b   (optional)
texture = zzmain-upperleft.tga
```

Skin textures used by the shipped actionbar/inventory are blank decoration; the art is in the per-widget middle textures.

## Action bar (`Actionbar.gui`, layout 1024x164, anchored bottom)

(rectangles in the two tables below are the parent-relative `clientRect`, as the heuristic reader reports them)

| widget | rect (parent relative) | purpose |
|---|---|---|
| picBackground | 0,0,1024,164 | `f3_actionbar.tga` (800x128 stretched) |
| btnWeapon01 / btnWeapon02 | 392,60,576,152 / 588,60,772,152 | weapon slot 1/2; children btnUp (`f3_actionbar_weap_up`), btnDown (`..._down`) |
| picWeaponNNIcon_up | 12,6,172,86 (in button) | weapon image `WP_*_ACT.tga` |
| lblWeaponNNMode_up | 4,2,156,26 | attack mode text ("SINGLE", "THROW"), font Arial |
| lblWeaponNNAP_up | 4,54,156,78 | "4AP" |
| picReloadIconNN_up | 160,74,166,78 | `f3_actionbar_ammo_on/off.tga` |
| picWeaponNNCalledShot_up | 137,55,157,75 | `f3_actionbar_calledshot.tga` |
| winHealthDigit1/10/100, winFatigueDigit*, winEvasionDigit* | x 342/328/314, y 30 / 79 / 131 (14x18) | three digit boxes on the left |
| picPlayer00..05 | 380+104*i,0,..,36 | party member frames (`f3_actionbar_PC_up/down`, fills `fill_yellow` health, `fill_blue` fatigue) |
| lblActionPoints, picAPIcon, Picture (AP_box) | 654,40 / 412,44 / 624,42 | AP counter and cells (`f3_actionbar_AP_*`) |
| btnInventory, btnCharacter, btnOptions | 788,92 / 908,92 / 908,128 (36x36) | `f3_button_up/down.tga` |

## Inventory (`Inventory.gui`, layout 1024x604; background `f3_inventory.tga` 800x472)

| widget | rect | purpose |
|---|---|---|
| winBackpack | 68,96,260,496 | item list: 3 columns x 6 rows of 60x60 cells (`picSlotRRCC`, icon at +4,+4 52x52, `picSlotRRCCHighlight` alpha 128) |
| btnBackpackUp/Down | 260,56 / 260,84 (24x24) | scroll |
| picDropToWorld | 244,528,292,576 | drop target (`f3_inv_drop.tga`) |
| picHeadSlot / picBodySlot / picHand1Slot / picHand2Slot | highlight rects (316,72) (316,212) (316,348) (316,488), 116x76; icon 76x76 at +20,0 | paperdoll equipment slots |
| winHeadEquip / winBodyEquip / winHand1Equip / winHand2Equip | x 468; y 164 / 216 / 268 / 320 (308x64) | per-slot readouts: name, armour thresholds (Norm, Elec, Heat, Bio, EMP, Ball), weapon range/ammo/damage |
| winAbilities | 468,68,532,152 | S P E C I A L values (lblST..lblLK / ...Value) |
| winSkills1 / winSkills2 | 532,68 / 644,68 | skill values |
| lblWeight, lblWeightValue | 68,60 | carry weight readout |
| lblDescriptionText, btnDescriptionUp/Down | 460,400,928,572 | item description with scroll |
| winModel | 772,28,988,376 | 3D paperdoll model |
| picSortPulldown + lblSortType/Weight/Number | 40,576 | sort menu |

## Fonts

`Fonts\r_fallouty.ttf` = "Fallouty" (numbers/values), `Fonts\TT0807M_.TTF` = "Gothic821 Cn BT" ("Gothic 821 Condensed BT",
labels/menus), `Fonts\JH_FALLOUT.TTF` = "JH_Fallout". "Arial" is a system font (the client falls back to Fallouty/Gothic).

## Item icons

Prototype `icon` (`BodVaultsuit_INV.dds`) / `icon_ground` (`WP_..._ACT.dds`) name TGAs of the same base name in the Interface
grp (`*_INV.tga` inventory icon, `*_ACT.tga` action bar weapon image).
