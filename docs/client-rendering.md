# Client rendering (original assets)

The raylib client (`engine/client`) can draw the original Van Buren maps from the installed game data. It sits
behind `IRenderer`; without a game directory the old placeholder box renderer is used.

## Running

```
f3_client --host 127.0.0.1 --port 4141          # network mode; the game dir is detected automatically
f3_client --viewmap 03_Tutorial_Junktown        # no server, map viewer
```

Network mode is 3D by default. Game directory detection order: `--gamedir`, env `F3_GAMEDIR`, `client.cfg` next to the
exe (or cwd), `F:\_work\F3_Demo`, `<root>\..\F3_Demo`, `<root>\F3_Demo`, `<root>`, a few common drives. A directory counts
when it has `resource.rht` and a data dir. The result (and host/port/name) is persisted to `client.cfg` next to the
exe. If nothing usable is found the flat placeholder is drawn with a red banner explaining why (no gamedir /
unusable gamedir / map missing from the game data) and how to fix it.

Test options: `--shot FILE.png`, `--shots PREFIX --shot-times 5,8,11` (timed series, logged with frame timestamps),
`--cam AZ,EL,FOV`, `--target X,Z`, `--clipy Y`, `--noentities`, `--modelscale S`, `--size W,H`, `--protos FILE`,
`--no-assets`, `--legacy` (do not announce the timing capability; tests per-cell CritterMove), `--say TEXT` (repeatable),
`--grid` (start with the G walk-grid overlay, drawn around the player, on top of the terrain), `--autowalk`, `--autoattack`, `--autopickup`, `--trace` (movement smoothness summary), `--run-seconds N`.
Note: under MSYS bash set `MSYS_NO_PATHCONV=1` or `/tp ...` is rewritten into a Windows path.

Controls (network): LMB acts according to the cursor mode, RMB click cycles the mode (Move/Attack/Use/Talk/Pickup, or
keys 1-5), Shift = walk instead of run, Tab stops attacking, I inventory, C character, X examine, Enter chat.
The camera follows the player; wheel zooms; RMB drag orbits; Q/E rotate.

## Movement and timing

Each critter has a timed polyline track sampled every frame, so motion is continuous. `MovePath` gives the polyline
with server timing; legacy `CritterMove` steps are chained one step duration apart. A replaced track keeps visual
continuity through an offset that decays in 0.2 s. `--trace` reports displacement / (speed*dt): mean 1.0000, sd < 0.001
for both protocols. Clicks send `MoveToF` (float destination) when the server answered `ClientCaps`, else `MoveTo`.
HUD: HP, AP cells (regenerating linearly from `WaitState`), and a break bar driven by the server wait-end time.

## Skeletal animation

Critters use the b3d mesh with the Granny skeleton and clips from `anim.gr2` (`engine/common/gr2`, a vendored copy of
`tools/gr2lib`), skinned on the CPU into dynamic meshes (`CritterRig`). Findings: in preconverted critter meshes the
"4 lightmap uv sets" block is really 4 skin influences per vertex `{u32 bone, f32 weight}` (bone = index in the b3d
bone list, 0xFFFFFFFF unused); b3d bone names equal Granny bone names; b3d units are exactly 10x Granny units.
Skin matrix (row vectors) = `inverseWorld[j] * poseWorld[j]`, skinned in b3d space then z negated. Clips are in place and
loop; `<prefix>_Com_Una_{Idle,Walk,Run,Punch_Jab,DmgFr,Laydown_ProneB}_01` (prefix HumM/HumF or CR_* model name), state
(idle/walk/run/attack/hit/death) is derived from critter state with a short blend. Model forward is original +z; yaw from
the movement direction.

## Protocol use and model ids

All `protocol_game.h` extension messages are handled (stats, inventory, container, object info, critter state, combat
events, door cells, timing). `ItemAdd.pid` is an index into the key-sorted `protos.json` (`ProtoDb`). `CritterAdd` carries
no model, so the client optionally accepts `S2C_CritterProto` (id 135: critter id, pid) sent after CritterAdd; without
it the model is looked up by critter name in protos.json, falling back to the player model.

## Libraries

`engine/common/gamedata` (`f3_gamedata`, STL only, no raylib) :

* `gd::GameData` - opens `resource.rht` + `data\*.grp`; `Find(name, type, grpHint)` is case-insensitive, strips a
  trailing extension when missing; `Read`, `List(type)`. Thread safe.
* `DecodeTga` - all variants (RLE, palette, 15/16/24/32 bpp, origin bits).
* `ParseB3d` - meshes, groups, shader/blend names, texture names (legacy and preconverted layouts).
* `ParseEmap`, `ParseMapData`, `ParseEntityTemplate`, `ParseTre`.
* `f3_gdtool <gamedir> scan | map <Name> | b3d <Name>` validates everything (3912 b3d, 39/39 tre, 38/38 emap,
  39/39 mapdata, about 0.6 s, 1.9 M triangles, 0 undecoded sub-records).

Client side: `GameAssets` (textures with mipmaps, B3D to raylib meshes, entity template resolution, shader),
`MapView` (tre batches, entity instances, walk grid, ground picking), `Shaders.h`.

## TRE geometry (new findings)

* `VTXD`: u32 buffer count, then `VTXB` chunks: u32 count and SoA sub-chunks `XYZ ` (3 f32), `NRML` (3 f32),
  `DIFU` (BGRA D3DCOLOR), `TXUV` (2 f32), `LMUV` (2 f32). Indices are u16, so large maps have several buffers
  (Junktown 4).
* Tree: `NODE`/`LEAF`/`IDXD` are header-only; their declared sizes are unreliable (some LEAFs 4 bytes short,
  zero padding in Vault). Only `INFO`/`IDXS`/`FLGS` are stepped by size; after unknown bytes resync by scanning for
  the next `NODE`/`LEAF`, bounded by file size (the TREE size is a few bytes short).
* `INFO`: 6 f32 bbox, then u32 at payload offset 24 = vertex buffer index. `FLGS`: one byte per triangle.
* `IDXS` is a u16 stream: `0x8000|material, len` groups containing `0x4000|(lightmapPage+1), len` sub-records
  (0 = no lightmap) containing runs. Run = `[textureIndex+1 (0 = none), count (multiple of 3), count absolute
  vertex indices]` followed by one 0 pad word when count is odd.
* Material index selects a MATD technique name (the MATR texture words are junk). Diffuse textures are the non-`.ctx`
  TXTD entries; lightmap pages are the `.ctx` entries in order, stored as `<Map>_<n>.tga` in the Maps grp.

## Coordinates and scale

Original data is left-handed, Y up (Direct3D). Renderer space is right-handed Y up: `(x, y, -z)`, triangle winding
flipped (indices 1 and 2 swapped). Back-face culling is on (ceilings face down, so interiors show as a dollhouse;
PgUp/PgDn cut roofs). World unit = metre; the walk grid has 2 cells per unit and starts at the tre root bbox min `(ox, oz)` (cell centre
`(ox+(cx+.5)/2, -(oz+(cy+.5)/2))`; see `formats/maps.md` section 1). B3D models are scaled by `MapView::modelScale = 0.3` (a human comes out about 1.8 m).
Entity yaw: `MatrixRotateY(-yaw)`. Server tile to world: `(ox+(tx+.5)*cw, 0, -(oz+(ty+.5)*ch))` with `cw = gridW*0.5/mapWidth`.

## Camera

Matches `sys.ini`: perspective, distance 350, fov 6..15 degrees (default 12), elevation 38..89, clip planes
30..1500. Position = target + d*(cos el sin az, sin el, cos el cos az); at az 0 the screen top is original +z.

## Shading

One GLSL 330 shader (`Shaders.h`): diffuse * vertex colour * 2 * lightmap * `lmGain` (2.0, the original "2X"
modulation). Dynamic props get a simple sun term. Material flags come from the technique name: `NO_TEXTURE*`
vertex colour only, `_LM_` lightmapped, `VERTEX_ALPHA` / `IMAGE_ALPHA` blended or cut-out (drawn last, depth write
off). Entity template (`<tmpl>.<TYPE>` in EME2) resolves through the `.een` to a model plus skin override.
Lightmaps/textures use trilinear + 8x anisotropy, 4x MSAA.

## Performance

Junktown (largest outdoor map): 157,602 triangles, 252 batches, 35 entities, loads in about 0.1 s after the
0.6 s one-off resource index; holds 60 fps (vsync-capped) at 1280x720 on an NVIDIA GPU. Not profiled uncapped.
Vault: 47 k triangles. All geometry is static GPU meshes; no per-frame allocation.

## Known gaps

* Critters are drawn as base body (plus skin override) only: no outfit/hair parts, no weapon-specific clips.
* Talk mode only examines (no dialogue); doors with a skeleton play `<model>_Open_In_01` (the slab sinks into the floor), doors without one are hidden while open.
* The server does not send `S2C_CritterProto` yet, so NPC models come from the name lookup.
* No particles, VFX, decals, water; no per-light dynamic lighting (baked lightmaps plus a sun term on props).
* Ground picking uses up-facing triangles only; multi-storey interiors pick the first hit from the camera.
* Original material state (ADD blends, fog, per-material alpha ref) is approximated by the flag mapping.
* 39 resources typed as TGA are actually BMP and are not decoded.

## Critter appearance, cursor, ground (update)

* `modelScale` is 1.0 (world unit about 1 foot, human about 6 units). Critters stand on a client height field (`MapView::GroundHeight`) built from up-facing tre triangles (0.25 unit samples, lowest layer, hole fill, 9x9 median, two [1 2 1] blurs, bilinear lookup); used for drawing, camera follow and picking. `F3_DUMP_HF=<file.pgm>` dumps it.
* Human skin: `Appearance{model, skin, worn}` composes a 512x512 atlas from the part textures (slots: skin 0,0/256; Bod 0,256/256; Sho 256,0/128; Hea 384,0/128; Han 384,128/128; Fee 384,256/128; Bac 256,256/128; Hai 256,128/64; Pon 288,224/32; Bea 256,192/32; Mus 256,224/32; Eye 320,192/64). Parts come from proto `sockets` plus worn armor (part names in `item.raw_hex`, `_LG` suffix = texture of preceding part). Base groups listed in a worn piece's `hides` (B3D node token 0x1b) are skipped.
* Cursor: RMB toggles Move <-> Examine; key A enters Attack (RMB/Esc leaves); Pickup / Use / Talk are automatic on hover (`ResolveCursor`); keys 1-5 removed. WASD pans only in the viewer.
* Walk/Run clip rates follow server speed (9/18 cells/s).
* Known gaps: a few wall rims remain in the height field; no dialogue engine (Talk examines).

## Doors and model unit scale (update)

* b3d meshes with `preconverted == false` are in 1/10 world units and are drawn x10 (`ModelAsset.unitScale`); preconverted meshes x1.
  `SkinModel.skelScale` is 10 for preconverted meshes and 1 otherwise. Door models used to be tiny and sunk because of this.
* Rigged doors (`MapView::SetRigs`, `Update`): CPU-skinned with the Granny skeleton from `<model>.mesh.gr2`, `doorT` moves 0..clip length
  towards the server open state.
* Known gaps: the Overseer door and other doors without skeleton just vanish when open; some Vault rooms are dark (lightmaps, pre-existing).

## Original-art action bar and inventory (`OrigUi`)

`engine/client/OrigUi.{h,cpp}` draws the action bar and the inventory window from the game install (`Interface\Actionbar.gui`,
`Inventory.gui`, `f3_*.tga`, item icons `*_INV.tga` / `*_ACT.tga`, fonts `Fonts\r_fallouty.ttf` + `TT0807M_.TTF`); layout format in
`docs/formats/gui.md`. Nothing is copied into the repo. If any of it is missing, `Load()` fails, a warning is logged and the old
raylib HUD is used; `--no-origui` forces the raylib HUD, `--open-inv` starts with the inventory open (screenshots).
Everything else (chat, dialogue, container, character sheet, labels) is still raylib.

* The layout is 1024 wide (bar 1024x164 anchored bottom centre, inventory 1024x604 centred above it); both scale down uniformly
  on small windows. Background textures are stretched (800 -> 1024), buttons are 1:1.
* **Action bar**: weapon slot 1 = hand 1 (active weapon, "down" texture), slot 2 = hand 2 (spare; clicking it swaps the hands via
  `Equip(id, 1, SlotHand1)`). Weapon image = `iconAct`, mode text = first attack mode of the prototype (SINGLE/BURST/THRUST/SWING/THROW,
  mode type numbers are a guess), `NAP` = mode AP word / 10 (guess), ammo icon on when the player carries any ammo and the weapon is
  ranged. Digit boxes: health, current AP, armor class. Player frame (name, yellow health fill, blue AP fill), AP cells (green full,
  blue regenerating, grey empty) and AP counter, Inventory / Character buttons. The dark message area on the left holds the cursor
  mode buttons (Move / Examine / Attack), the break-time bar (red while actions are locked) and the busy / no-AP message.
* **Inventory**: backpack = 3x6 cells of unequipped items (scroll with the wheel or the arrows, sort by type / weight / name by clicking the
  field below the list), paperdoll slots Head / Body / Hand 1 / Hand 2, per-slot readouts (weapon damage, range, ammo; armor threshold /
  AC), SPECIAL, HP, AC, level, weight `carried / limit` (red when over), description box.
  Interaction: drag an item onto a slot of the right class (valid slots get a green frame) to equip, drag an equipped item onto the backpack
  list to unequip, drag onto the trash can or out of the window to drop, double click equips / unequips, right click examines (name +
  description, weight, weapon / armor data are shown in the description box), hovering shows a tooltip. Item names and descriptions
  come from English.stf through `protos.json`.
* Not done: skills, the 3D paperdoll model (`winModel`), "use item from inventory", attack-mode switching, reload.
