# Van Buren tech demo - world and logic formats (maps, entity templates, scripts)

Companion to `containers.md` (grp / rht / stf) and `models.md` (b3d). Everything here was derived from the shipped demo data
(`extracted/<grp>/<name>.<ext>`, produced by `tools/grptool`) plus the F3.exe code through REA's Ghidra provider (the GhidraMCP
backend was down; function addresses below are F3.exe virtual addresses, all functions unnamed `FUN_xxxxxxxx`).
`tools/mapinfo` implements every table in this file strictly (any unexpected byte = parse error) and was run over the whole demo:

```
mapinfo verify extracted          # maps 42/42 (39 tre+mapdata, 38 emap), emap 38/38, mapdata 39/39, tre 39/39,
                                  # entity templates (.een) 285/285, AMX scripts 119/119
```

Integers are little-endian. `str` = `u16 length` + bytes (no NUL), Windows-1252. Floats are IEEE f32.
Resource type names (from the user's Van Buren Explorer): 300 SMA, 400 TRE, 1200 SCR, 1300 MAP, 1400 CRITTER, 1500 VFX, 1700 COL,
1800 ITEM, 2400 WEAPON, 2500 ARMOR, 2600 DOOR, 2700 USE, 3500 AMMO, 3600 CON. grptool extensions: `.scra`=300 `.scrb`=1200,
`.tre`=400, `.emap`=1300, `.mapdata`=1700, `.een`=1400/1800/2400-3600.

## 0. Big picture

| file (per map) | type | holds |
|---|---|---|
| `<Map>.tre` | 400 TRE | baked static level: lights, materials, textures, **all tile/scenery geometry already merged into one vertex pool**, spatial tree, level grid. *Tiles are not referenced individually by the map.* |
| `<Map>.mapdata` | 1700 **COL** | per-cell RLE grid: collision / surface class (section 2) |
| `<prefix><Map>.emap` | 1300 MAP | **dynamic content**: entities (critters, doors, containers, usables, items, weapons...), entry points, trigger regions, transitions, waypoint paths, sound/effect emitters, water, camera |
| `Scripts/*.scra/.scrb` | 300/1200 | compiled Pawn (AMX) scripts |
| `_CRT/_WEA/... *.een` | 1400..3600 | entity templates ("EEN2"): stats + defaults |

emap file name prefixes (`00_`, `04_`, `98_`, `zz_TestMaps`) are just the original folder names flattened; the real link is the first
string in the emap header (`<tre base>.8`; match case-insensitively - `Kihantest` vs file `KihanTest`). 38 emaps, 39 tre/mapdata:
`0301_Mining_Cave_2`, `0501_Chiefs_Fallout_Shelter_Surface`, `Test_City_Building05_b`, `Test_Ext5x5` and `KihanTest`(case alias)
have no emap of their own; `Test_Building`, `Test_Junktown_fences_metas`, `Weapon_Ammo_Item_Test1` have an emap but no tre/mapdata.
`Engine/Default_StartMap.*` is a full triple that lives in Engine.grp. `sys.ini` (Engine) says `[Server] Start map =
00_03_Tutorial_Junktown.map`, `Start map entry point = 0` and `[Mainmenu] map name = mainmenu.map` - the `.map` suffix is the emap name.

## 1. World coordinates, grid, units

* **Y is up**; world position = `(x, y, z)` floats, the ground plane is x/z (entity y is height: 0 on the floor, 2.5 for windows).
* **Grid resolution: 2 cells per world unit.** Evidence: (a) tre `LVLD/INFO` is `(w, h, 0.5, 0.5)` with the same w,h as the mapdata;
  (b) the root bounding box of the spatial tree equals `w/2 x h/2` (Junktown mapdata 384x624 -> bbox x -1..191, z -1..311;
  Test_Brian 84x84 -> 4..46; Mainmenu 380x204 -> x -95..95); (c) every entity/entry point lies inside `[0,w/2] x [0,h/2]`
  (Junktown entities x 26..161, z 38..289). Cell for world point: `(floor(2x), floor(2z))`, row = z. Mainmenu is the exception (tre
  bbox is centred on x=0), origin offset of that map is unknown.
* Tile size: `Ext5x5_*` tiles are 5x5 world units = 10x10 cells; `Int10x10_*` are 10x10 units = 20x20 cells (grid dims are always
  `4 + 10*k` for 5x5 sets: Test_Ext5x5_City1 174x114 -> 17x11 tiles; Sewers 624x624 -> 31x31 tiles of 10). b3d tile meshes are
  stored at 1/10 of that (`models.md`: 5x5 tile = 0.5 file units), so **world units = b3d file units x 10** (inferred from the grid
  arithmetic; not seen in code).
* Instances carry **yaw only**: in all 175 map entities the rotation triple is `(yaw, 0, 0)` (radians, range about -pi..pi, rotation about
  Y) and the EEOV scale is always 1.0. There is no per-instance scale/pitch/roll in the demo data.
* `sys.ini [World] Seconds Per Round = 6`, date 2253-10-18 13:13:13; `[CharGen]`/camera values in the same file use the same
  x/y/z convention.

## 2. `.mapdata` (type 1700 COL): RLE collision/surface grid

```
u32 width, u32 height
for each row y in 0..height-1:            // row-major, row index = world z*2
    repeat until the row has `width` cells:
        u32 run_length   (>=1, never crosses a row)
        u8  value
// file ends exactly after the last row (verified for all 39)
```
(Earlier guess of a 3-u32 header was wrong: the third u32 of a hex dump is the first run length.)

Values seen over the 39 maps (cells): `0x2f` 1,981,523; `0x01` 501,785; `0x02` 383,381; `0x05` 179,151; `0x09` 175,961; `0x00` 234,939;
`0x2c` 21,297; `0x16` 10,182; `0x18` 7,135; `0x1b` 4,387; `0x04`, `0x23`, `0x1f`, `0x26`, `0x22`, `0x17`, `0x2a`, `0x21`, `0x1a`, `0x0b`, `0x0c`, `0x10` rare.

* `0x2f` (47) = **blocked / no floor**: it fills the outside of every map and the 2-cell border (Test_Brian: border `0x2f`, interior `0x02`;
  84x84 cells -> 81x81 open), forms the building footprints (Test_Ext5x5 has a 11x12-cell hole of `0x2f` in a field of `0x09`) and is what
  door entities (6 of 8 checked) and containers stand on.
* The other values are **walkable ground classes** (surface/material ids): critters, items and every player entry slot start on
  `0x01/0x02/0x05/0x09/0x00` (151 of 163 points checked; the rest sit on props that carry `0x2c`/`0x2f`). Per-map dominant value follows
  the tile set (`0x01` desert/mesa, `0x09` city asphalt/concrete, `0x05` vault metal, `0x02` cave/test, `0x00` mines/sewer).
* **Unknown:** the exact enumeration of values (`0x2c` forms thin scatter next to props = prop footprint/obstacle; `0x22`, `0x17`, `0x1b`,
  `0x26` appear as small patches), and whether non-`0x2f` classes also encode walkable-but-slow or footstep sound. The sound
  scripts `FX_Foot_Hum_{Asphalt,Carpet,Metal,Sand,Wood}_Base` suggest surface classes feed footsteps. F3.exe's mapdata loader was not located
  (no `.rle`/`mapdata` string; the emap header names the file `<Map>.rle`, an older extension).

For a server: use this grid as the walkability bitmap (`blocked = (v == 0x2f)`), 2 cells per world unit. Dynamic blockers (doors, props with
`0x2c` footprints) are inside the grid already.

## 3. `.emap` (type 1300 MAP)

```
char[4] "EMAP"; u32 version = 5; u32 header_size; u32 0
str tre_name      // "<tre base>.8"  (".8" is a legacy extension token)
str rle_name      // "<tre base>.rle"
str automap_name  // "<tre base>_AM.dds" (the shipped file is <tre base>_AM.tga in Maps.grp)
27 bytes          // see below;  header_size covers all of the above
then chunks until EOF:
    char[4] tag; u32 chunk_version; u32 total_size /* includes these 12 bytes */; payload
```
27-byte header tail: `u16 0, u8 rgb[3] (ambient/background colour: 46 42 39 vault, ffffff test maps, 53 58 6c junktown), u8[4], u8 (0x80/0x60/0x64),
u8 0/1, u8[4], f32 250.0, f32 500.0, f32 1.0` (fog/clip-like floats; Mainmenu `0,0,1`). Field semantics unverified; `mapinfo` prints the raw hex and last 3 floats.

### Chunk table (counts over the 38 emaps)

| tag | n | payload |
|---|---|---|
| `EME2` | 175 | entity instance (below) |
| `EMEP` | 14 | entry points: `u8 id` then `n=(len-1)/16` slots: `n*(f32 x,y,z)` then `n*f32 yaw`. 6 slots (party positions) normally (yaw 0 or pi), 1 slot in Test_Mesa_2. Junktown has ids 1 and 2, Vault id 1. |
| `ECAM` | 38 | `f32[4]` camera/start data (Junktown `8.32 296.8 178.7 9.22`; test maps `0 1000 1000 0`); semantics unknown |
| `EMNP` | 38 | `u32` always 0 in the demo (probably count of nav points) |
| `EMTR` | 28 | region polygon: `u32 kind; u32 n; n*(f32 x,y,z)`. kind 6 = banded volume with constant `y` (8.0, 9.25, 10.25: building floors/roofs, followed by `EBTR`), kind 1 = script trigger (followed by `ESTR`), kind 0 = map transition (followed by `ETTR`). The polygon is in world units, in the x/z plane. |
| `EBTR` | 23 | `u32 id; u8 rgb[3]` (id 1..5, colour 0x464646) - attaches to the preceding `EMTR` (kind 6) |
| `ESTR` | 3 | `str script ("00_Vault_Door_Trigger.amx"); u32 0` - attaches to preceding `EMTR`; script is `Scripts/<name>.scrb` with `OnEnter/OnLeave` publics |
| `ETTR` | 2 (v1) | `str target_map ("00_04_Tutorial_Vault"); u8 target_entry_point; u8 flag(1)` - attaches to preceding `EMTR` kind 0. Junktown->Vault entry 1, Vault->Junktown entry 2 (matches the EMEP ids). |
| `EPTH` | 9 | `str name ("Waypoint01"); u32 n; n*(f32 x,y,z + f32 e0,e1,e2)`; e = (wait/heading-like float, 0, 0) |
| `EMSD` | 23 | sound emitter: `str name; f32 x,y,z; str sound ("AMB_WindBlowing.psf"); u8 1; u8 1` |
| `EMEF` | 35 | effect emitter: `str name; f32 pos[3]; f32 rot[3]; u8 1; str vfx ("FX_FireExtraLarge_01.veg"); u8 1` |
| `EMNO` | 5 | area-map icon: `f32 pos[3]; str icon ("AreaMapIcon01.ddst"); u32 7` |
| `2MWT` | 2 | water plane (CanyonWater01, ScottE_Test2): `str "EFF_Water"`, then a ~1-2 KB parameter block of f32 + a grid of `(f32 u, f32 v, f32 x?, f32 y, f32 z, str texture)` records. Only skipped by `mapinfo` (kept as raw size). |

### `EME2` entity instance (verified 175/175)

```
str    name                  // "<template>.<TYPE>"   e.g. "Critters00_TMCpl_Armstrong.CRT", "Weapons44_Revolver.WEA"
f32    x, y, z               // world position
f32    yaw, 0, 0             // radians; only the first is ever non-zero
u8     1
EEOV chunk (below)           // per-instance overrides; the EME2 ends exactly after it
```
`TYPE` selects the template folder: `CRT`->`_CRT` (also used for barrels/windows: "Useable_ObjectsExploding_Barrel.CRT" and
`DS_PropsCity1Build01_Window.CRT` are critter-class templates with scripts), `CON`->`_CON`, `USE`->`_USE`, `DOR`->`_DOR`, `ITM`->`_ITM`,
`WEA`->`_WEA`, `ARM`->`_ARM`, `AMO`->`_AMO`. Template file = `extracted/<_GRP>/<template>.een`.
Resolution in the demo: 157 of 175 template names resolve; the 18 that do not are stale references in test maps
(`Ammo.22.AMO`.. in Weapon_Ammo_Item_Test1, `zzMarkTest*`, `zzDaveTest`, `CrittersGila_Monster`, and the three `DS_Props...ITM` in
`02_05_Chiefs_Fallout_Shelter`/`98_Canyon_Random_01`). 173/175 `.amx` references exist as `Scripts/*.scrb` (`dave.amx`, `<No Script>` missing).

Entity count by type: CRT 85, WEA 33, AMO 18, DOR 16, CON 13, ITM 5, USE 3, ARM 2.

### `EEOV` entity override block (shared by `.emap` instances and `.een` templates)

Evidence: writer FUN_0054f6a0 (0x54f6a0) read against 175 instance + 285 template blocks, 460/460 consumed exactly.
```
char[4] "EEOV"; u32 version (1 or 2); u32 total_size
str  instance_name       // "<ShortName><Group>_<NNN>" e.g. "Cpl_Armstrong00_TM_000", "Box_BlueDS_Vault_000"
u8   b0, b1, b2          // always 1,1,1
u32  team                // 0..9; Cpl_Armstrong 1, insurgents 2, Frank 3 (scripts have GetTeam/SetTeamAttitude)
u32  B                   // 0 (one template: 1)
str  dialog_script       // "00 Corporal Armstrong"  -> Scripts/<name>.scra (conversation) or ""
str  ai_script           // "CaptainArmstrong.amx"    -> Scripts/<name>.scrb (event script) or ""
u32  n; n*{str name; u32 value}     // named values, always empty in the demo
u8   f179                // 1 for critters/items (121/175), 0 for most doors/containers
f32  scale               // always 1.0
str  skin                // texture override, e.g. "Skin_HumM_Black01_LG.dds"
u8   nfx; nfx*str        // effect (.veg) names, e.g. "FX_ElectricalTechGeneratorSmoking_01.veg"
u8   f178                // 1
u8   f17a                // 0 (6 instances 1)
if version >= 2: u32 n; n*str     // inventory/contents, "<name>.<TYPE>" e.g. "Ammo44.AMO", "Weapons44_Revolver.WEA"
```
Version 1 (6+33+18+3+2 test-map instances) lacks the inventory list. Container contents (`CON`) and NPC loadouts live here.

## 4. `.tre` (type 400 TRE): baked level

```
"8TRE"; u32 size                       // payload size; file may carry (size+8 .. EOF) trailing bytes (16..1356 bytes; IDXD/IDXS tails
                                       // in some maps, 0x01 filler in others) - ignored
chunks: char[4] tag; u32 payload_size; payload
HEAD (16)  u32 version=2, u32 n0, u32 n1, u32 n2     // (12,6,510) Junktown, (12,4,75) Vault, (14,5,71) Mainmenu: unknown counts
LGTD       { INFO u32 count; LGTR x count }          // lights
LVLD       { INFO {u32 w, u32 h, f32 0.5, f32 0.5}; LVL0 blob }
MATD       { INFO u32 count; MATR x count }          // 196 bytes each: u32 id; char name[32] (shader technique, e.g. SK_BASE_LM_2X,
                                                      //  BASE_2X, BASE_LM_2X, NO_TEXTURE_VERTEX_ALPHA); 160 bytes params (not decoded)
TXTD       u32 n_a, u32 n_b, then entries {"TXTR" u32 len; cstr name}   // texture names (.tga/.ctx), counts differ from n_a/n_b
VTXD       u32 1; "VTXB"; u32 size; u32 vertex_count; ...  // size == vertex_count*44 + 44: 44-byte vertices (see models.md vertex format)
TREE       "NODE"{ "INFO"{ f32 bbox_min[3], f32 bbox_max[3], ... }, children "LEAF"{INFO,IDXD,IDXS...} }   // spatial tree for render/culling
```
`LGTR` (64 bytes): `u32 type (0 = directional "sun" with a far position, 2 = point light); u32 mode; f32 color[3]; f32 pos[3];
f32 dir[3] (NaN when unused); f32 params[3] (falloff 0.45, radius 15..22.5, 1.0); f32[2] 0`. (Mainmenu: sun at (-309.6, 306.8, -309.6).)
`LVL0` = sparse stream of `(u32 index, u32 bitmask)` pairs whose leading run has the constant `cells/4 + 4`; it is a per-level occupancy/visibility bit set
for the w x h grid; **not decoded**. F3.exe strings (`Octree2Gfx::RenderFloorCB`, "Failed to get Static Mesh Data",
"Node's IndexCount exceeds max") show the TREE is consumed by an octree renderer that also owns the floor triangles used for picking;
there is no separate collision mesh - the COL grid (section 2) is the collision data.
So **tile placement is baked**: to rebuild a map from `.tre` you read VTXD/IDX data directly; the Tiles.grp `{n}` meshes are only the editor palette.

## 5. Entity templates `.een` (EEN2)

All 285 files (`_CRT` 108, `_WEA` 51, `_CON` 48, `_ARM` 22, `_AMO` 20, `_USE` 18, `_DOR` 13, `_ITM` 5) are a sequence of
`char[4] tag; u32 version; u32 total_size` chunks:

| class | chunks |
|---|---|
| `_CRT` | EEN2, GENT(v1), GCRE(v4) |
| `_WEA` | EEN2, GENT, GITM(v1), GIWP(v1) |
| `_ARM` | EEN2, GENT, GITM, GIAR(v0) |
| `_AMO` | EEN2, GENT, GITM, GIAM(v2) |
| `_ITM` | EEN2, GENT, GITM |
| `_USE` | EEN2, GENT, GOBJ(v0) |
| `_DOR` | EEN2, GENT, GOBJ, GDOR(v1) |
| `_CON` | EEN2, GENT, GOBJ, GCON(v1) |

**EEN2** (v0): `str model; str icon_inventory; str icon_action; u8 flags[5]; EEOV` - `model` is the b3d resource name (verified: critter
`HumMNor`, prop `DS_Tech_Props{7}Computer02`, weapon `GD_WP_15mmArtemisRailGun`); weapons carry `WP_..._INV.dds` / `_ACT.dds`
inventory/ground icons, armor/ammo only the inventory icon. `flags[5]` = (1,1,0,0,0) for most, doors (1,0,0,0,0).
The EEOV inside is the template's default instance (default script, team, skin, inventory).

**GENT** (v1, 8 x u32): `name_id, desc_id, name2_id, 0, 0, 0, 15|1|100, 15|1|100` - ids are **1-based indices into English.stf** (0 = none);
`stf[name_id-1]`: Frank -> "Frank", description -> "Frank appears to be a very nervous vault dweller, indeed.", type -> "Male Vault Dweller";
`WP_15mmArtemis` -> "15mm ARTEMIS Rail Gun"; last two words 15,15 (critters), 1,1 (items), 100,100 (Default_PC).

**GCRE** (creature stats, data v4; reader chain FUN_0040e530 -> FUN_0040e360 -> FUN_0040b6a0 / 0x409410 / 0x40d9e0 / 0x409700 / 0x40b9e0; layout verified on all 108 `_CRT`):
```
u32 special[7]                       // 5,5,5,5,5,5,5 for Frank; 6s for Default_PC (S,P,E,C,I,A,L order assumed)
u32 w7..w14                          // [level?(1..6), 3000|500|50|10|0, 250|50|10|0, 5|3|0, hp-like 10..30, 0|1|5, 1..6, 0|7]
u32 n; n*(u32 idx,u32 val)           // derived-stat overrides, idx 0..12 (Default_PC: (2,80)(0,80)(1,80)(7,75)(4,75): skill-like %), <=13 entries
u32 n; n*(u32 idx,u32 val)           // 82-entry table (always empty in demo; skills)
u32 n; n*u32                         // list (empty)
u32 n; n*u32                         // list: values 0..11 (48 critters use 8, 40 use 1, 23 use 11) - trait/perk/flag ids? unknown
u32 x274
str portrait ("port_bobby.dds"); u32 10|15|20; str ""; u32 0; u8 0
str[12] socket_a                     // all empty in demo
12 * (str model, str texture)        // appearance sockets 0..11 as in Engine/model.ini ([0]=BaseBody, [1]=Head ("HaiBowl"+"HaiBowl_red_LG.dds")...)
u8 socket_visible[12]                // 0,0,0,0,0,0,1,1,1,1,1,1 typical
u32 n_attack_modes
u32 base_uncalled_chances[8]         // game checks they sum to exactly 100 ("baseUncalledChances don't add up", F3.exe string)
u32 six[6]; u32 six[6]               // (0,0,0,100,0,0) twice for 84 critters
u32 n; n*str                         // starting equipment/inventory ("ClothingVault_Suit.ARM")
n_attack_modes * GWAM chunk (v5, 80 bytes): u32 m[11]; u8; u32; u8; u32; u8; u8; u32; u32; str; u8; u8
                                     // animals: m[9],m[10] look like min/max damage (Brahmin 2-4, Dog 1-3, Devil Dog 10-20); m[0]=10,m[1]=4 constant
```
The v5 writer (FUN_0040bb80) additionally stores 13-entry and 82-entry (index,value) tables with fixed counts and a `+0xd8` word; the
shipped data is v4 so those collapse to the counted lists above. Field *names* beyond SPECIAL are guesses; values are exact.

**GITM** (items; weapons 100-112 bytes, armor 111-194, ammo 80): `u32 class (0 misc,1 ammo,2 armor,4 weapon); u32 subclass; u32; u32 ...`,
then a socket table with per-body-part model names (`BodLeatherOutfit`, `HanLeather..`, `FeeLeather..`, `ShoLeather..`, helmets `HeaMotorcycle`),
and a trailing `str model` (weapons: "WP_223Autoloader"). **GIWP** 142-154 bytes weapon data (starts with an embedded `GWAM` attack-mode chunk;
weapon class, damage etc.), **GIAR** 28-40 bytes (7-10 words, armor values such as 4,4,4,0,8,4,0), **GIAM** 36-48 bytes (ammo: caliber/damage
modifiers e.g. `5, 12, -25`), **GOBJ** 28 bytes (`u32 kind` 0 usable/1 door/2 container then zeros), **GDOR** 12, **GCON** 14: **not decoded**
(`mapinfo een --json` prints words, embedded strings and hex for them).

## 6. Scripts (`.scra` type 300 SMA, `.scrb` type 1200 SCR): Pawn AMX bytecode

Both are standard **Pawn/Small AMX** files (magic `0xF1E0`, file_version 6, amx_version 6, flags 2 = debug flag set but no debug
section: `hea == size`), `defsize = 40` (public/native entries are `u32 address; char name[36]`, names inline). The "first entries
were readable text" are `globals.ini`/`locals.ini`, not scripts. 119 scripts (36 scra, 83 scrb); source is **not** shipped.

* `.scrb` ("behaviour/area/mob", 83): publics `OnCreate OnLoad OnUpdate OnMessage OnDamage OnDeath OnDestroy UseSkill GetUsableSkills ShowSkills`
  (all 78 object scripts), plus `OnAttacked` (critters), `OnOpen/OnClose` (doors), `OnEnter/OnLeave` (triggers), `OnStartDialogue`.
  `Template_MOB/DOOR/TRIGGER/AREA.scrb` are the base templates; per-map scripts (`Tutorial_Junktown.scrb`) are area scripts, `Player.scrb` runs the PC,
  `Conditions.scrb` (only `Dummy`) is the global condition script (`?conditions.amx` string in F3.exe).
* `.scra` ("conversation", 36): publics are dialogue graph nodes: `N<k>` node, `NA/NC/NU/NTA/NTC<k>`, `L[A|C]<i>_R<j>_N<k>`, `R[A|C|L]<j>_N<k>`;
  natives `RegisterNodeText, RegisterReply, SetCurrentNodeName, SetNodeTextFunction, AddEntry, PCSpeaker, StartDialogue, StartStore`.
* **115 distinct natives** (the engine script API), most common: Myself, RandomNumber, GetGlobal/SetGlobal, GetCurrentHP/GetMaxHP/AddHP, GetGender, GetRace,
  SetValue, SendMessage/MessageArgCount/MessageGetArg, UnlockDoor/LockDoor/IsLocked, DisplayStringRef, FloatingDamageText/FloatingSpeechText,
  ClearActions, GetTimeStamp/SecondsPassed/Hour/Minute, GetTotalSkill, GetPrimaryAttribute, AttachSound/RemoveSound, Protagonist, GetObject/FindObjects/FindEntity,
  Distance/PointDistance, GetLOS/GetLOSPoint, GetPosition/GetFacing/SetFacing/SetFacingTarget/GetDirectionToEntity, IsDead/HasDied/IsIdle,
  Team/Squad API (CreateTeam, AddToTeam, GetTeamAttitude, SetTeamAttitude, GetTeamRank, CreateSquad, AddToSquad, GetSquadID, SetSquadAttitude, SetSquadRank, GetSquadRank, FindTeamMembers, SendMessageToTeam),
  actions `A_Wait A_MoveToPosition A_MoveToEntity A_Attack A_JustAttack A_Wander A_PlayAnim A_FloatingSpeechText A_SendMessage A_Kill A_ReloadWeapon`,
  tasks `OpenTask CloseTask`, `GetWayPointCount/GetWayPointPosition` (EPTH paths), `GetNavPointCount/GetNavPointPosition`, `GetNearestPath`,
  items `HasItem GiveNewItem PartyHasItem IsItemEquipped EquipItem`, effects `AttachVEG RemoveVEG GetVEGID MapEffectSetActive MapEffectResetElapsedTime`,
  `GetMapID`, `IsEntityInArea`, `DestroyEntity`, `ActivateEntity/DeactivateEntity`, `SetModel`, `IsUnderFOW`, `IsInDialogue`, `IsInParty`, float helpers (`float floatadd ... floatround`).
  `mapinfo amx --json file` lists publics (with code offsets) and natives per script.
* `Scripts/globals.ini` (`[Globals]`, 669 lines): `Name=initial value ;comment` global variables and `SKILLDIFF*` constants (10,30,50,70,90,110,130);
  many names (`02_Aldcrown*`, `Archendale_Positive`) are leftovers from a different (D&D-style) project. `locals.ini` is empty.
* Not done: Pawn opcode-level disassembly/VM (standard Pawn 3.x instruction set applies; the native ids are call indices into the `natives` table).

## 7. Other data touched

* `Engine/sys.ini` (see section 0/1), `Engine/model.ini` (b3d socket table), `Interface/*.ini`, `Critters/*.ini` (`[ANIM_CUES]`, one per animation).
* Player templates: F3.exe hard-codes `Default_PC.CRT` and `PCFemale.CRT` (both exist in `_CRT`, SPECIAL 6 each, 2 attack modes, `Player.amx`);
  no `.CRT` player save files exist in this install (My Documents\F3 absent), save/game-state format not examined.

## 8. Recommended server-side data model (FOnline-style)

* **Map prototype** (static, loaded from COL+EMAP at build time): `{ id, name, w_cells, h_cells (2 cells/unit), walk[] (u8 per cell from mapdata; 0x2f blocked),
  entry_points[id] -> 6 slots (x,z,yaw), regions[] (polygon, kind: transition{map,entry}|script_trigger{script}|volume{y}), paths[], objects[] }`.
  Ignore TRE on the server except `tree bbox` (world size); it is render-only.
* **Object prototype** (from `.een`): `proto_id = (type, template)`; fields: stf name/desc ids, model, class (critter/item/weapon/armor/ammo/door/container/usable),
  default script, team, skin, default inventory; critters add SPECIAL[7], stat words, attack modes (GWAM), portrait, sockets, starting equipment; items add GITM/GIWP/GIAR/GIAM payloads (store the raw words until decoded).
* **Object instance** (from `EME2`): `{ uid, proto, map, x, y(height), z, yaw, team, ai_script, dialog_script, skin, effects[], inventory[] }`. Persist instance state
  (hp, inventory, locks) separately; the 4 named-value pairs/flags are unused in the demo.
* **Scripts**: keep the AMX files as the authoritative behaviour; implement the 115 natives behind a Pawn VM (or transpile) and call the publics `OnCreate/OnLoad/OnUpdate/OnMessage/
  OnDamage/OnDeath/OnDestroy/UseSkill/OnOpen/OnClose/OnEnter/OnLeave/OnStartDialogue`; dialogue = call `.scra` node publics. Globals from `globals.ini`.
* Grid/tick: 6-second combat rounds (`Seconds Per Round`), movement on the 0.5-unit grid, yaw continuous. Use `mapinfo map <dir> <name> --json` as the importer's input format.

## 9. Still unknown / not done

1. `mapdata` value enumeration (surface vs collision classes) and the exact loader (code not located).
2. `.tre`: `LVL0` encoding, `HEAD` counts, `MATR` parameter block, vertex/index payload decoding (needed only for client rendering; models.md has the b3d vertex format).
3. `EMTR` kind 6 semantics (building volume / roof fade is a guess), `ECAM`, `EMNP`, `EPTH` extras, header 27-byte tail, `2MWT` water layout.
4. GCRE field names (only SPECIAL is certain), GITM/GIWP/GIAR/GIAM/GOBJ/GDOR/GCON bodies, GWAM semantics, `EEOV` flags `f178/f179/f17a`.
5. AMX disassembly, native signatures/semantics (only names known), `.sndscript`/`.veg` formats.
6. 18 stale template references and 2 missing scripts in test maps are real dangling references in the shipped data, not parser errors.

See also [client-rendering.md](../client-rendering.md) for the decoded TRE geometry layout (VTXD/IDXS) and how the client renders it.
