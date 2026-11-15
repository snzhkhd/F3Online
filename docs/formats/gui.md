# Interface layouts (`Interface\*.gui`, resource type 1600) and skins (`*.ini`, type 800)

Status: **partially reverse engineered** (by inspecting the files; F3.exe was not needed). The reader is
`engine/common/gamedata/GuiFile.{h,cpp}`, a dump tool is `f3_gdtool <gamedir> gui Inventory`.

## .gui layout file

Little endian, strings are `u32 length` + bytes (no terminator).

```
"GUI V1.3"                      8 bytes magic
string  "Actionbar.INT"         layout name
...root window record...
widget records, depth first (the tree structure itself is NOT decoded, see below)
```

A widget record, as far as it is understood:

```
[rect4]            4 x i32  left, top, right, bottom (repeated copy of the rect below; the record starts here)
string name        "btnWeapon02", "picSlot0000", "lblWeightValue", ...
u8 flags[3]        [0]=1: "down"/disabled state variant, [1]=1: container (has children), [2]=1: skinned widget
                   (button/picture/window); labels are 000000
string skin        skin ini, e.g. "zzmainmenu.ini" (also "areamap.ini", "quickslot.ini", "f3_green.ini")
u8 r,g,b ; u8 alpha   tint (128,128,128 = neutral) and alpha (0 on invisible containers/buttons, 180 on labels)
i32 left, top, right, bottom   rectangle RELATIVE TO THE PARENT widget
... 3 x u32 flags, two u32 colours (0xFFFFFFFF text colour...), sizes ...   (not decoded)
9 x fragment record: [rect] u32 flags, string texture, u32 crc, rgb   -- the nine skin pieces
misc strings       font name ("Arial", "Fallouty", "Gothic 821 Condensed BT"), sound files
                   ("f3_button_down_01.wav", "f3_button_up_01.wav")
```

* The nine fragments are in the order UL, U, UR, L, M, R, LL, LO, LR. For a `pic*`/`btn*` widget the *middle* fragment is
  the picture itself (`picBackground` -> `f3_actionbar.tga`, `btnUp` -> `f3_actionbar_weap_up.tga`); the other eight come
  from the skin ini (`zzmain*.tga` nine-patch pieces, which are blank here) so only the middle matters for these widgets.
  Dynamic pictures (item icons, portraits) keep the blank skin middle and are filled by the game code.
* Textures are authored at 800x600-ish and the layout is 1024x768-ish: `picBackground` is 1024x164 but its texture
  `f3_actionbar.tga` is 800x128 (scale 1.28). Buttons (`f3_button_up.tga` 36x36 for a 36x36 rect) are 1:1.
* The nesting is not stored in a way the reader decodes. Records follow depth first; children coordinates are relative to
  the parent. The UI layer therefore finds a widget by name *after* its parent widget (`GuiFile::Find(name, from)`) and adds
  the parent origin itself. Negative rects (`picWeapon02Mask_up` at -172,-80) are mask/offscreen helpers.
* Strings of a skin may live in the grp resources (`Interface` grp: .gui 25 files, .tga, .bmp, .ini).

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
