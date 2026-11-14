# Van Buren tech demo (F3.exe) - 3D model, texture and animation formats

Companion to `containers.md` (resource ids, grp/rht). All integers little-endian. Everything here is implemented by
`tools/modelinfo` (C++17, no dependencies) and was run over the whole extracted demo (`extracted/<grp>/<name>.<ext>`).

```
modelinfo b3d  <file|dir>... [--obj <outdir>] [--raw] [-q]    dump / export .obj+.mtl (aggregate stats for >1 file)
modelinfo tga  <file|dir>... [--png out.png] [-q]             header dump, decode (RLE/palette aware), PNG export
modelinfo gr2  <file|dir>... [--dll granny2.dll] [--nodll]    Granny2 container inspector (+ object counts via the DLL)
modelinfo scan <extracted-dir>                                success-rate summary for b3d + tga + gr2
```
Build: `cmake -S tools/modelinfo -B build/modelinfo -A x64` (b3d/tga/gr2-container) or `-A Win32` (additionally loads the
32-bit `granny2.dll` for GR2 object counts; default path `F:/_work/F3_Demo/granny2.dll`, override with `--dll`).

## Results (full demo)

| format | files | parsed | notes |
|---|---|---|---|
| `.b3d` | 3915 | **3912 (99.92 %)** | whole file consumed to the EOF token, every index < vertex count, no trailing bytes |
| `.tga` | 1614 | **1614 (100 %)** | all decode to RGBA8 |
| `.gr2` | 547 | **547 (100 %)** container; 547 object-count queries via granny2.dll | payload is Oodle0-compressed (see below) |

The 3 failing b3d (`CR_Badger`, `CR_Bear`, `CR_Sheep`) use a different token numbering (file starts `35 01` instead of
`1c 01`); F3.exe itself rejects them (default case of the token switch throws "invalid token"), so they are dead data.
`CR_Monsta` is not a b3d at all (text `# G3D File`, parsed by a different reader; not covered).
OBJ export of all 3912 parsable models takes ~9 s.

## 1. B3D ("B3D 1.1 ")

Reader in F3.exe: `G3D_Mesh::ReadB3D` family. Evidence (addresses in F3.exe, functions are unnamed in Ghidra):

* `FUN_004de120` (strings "Not a B3D" 0x6720f8, "EOF token not found") - top-level token loop, `fread(8)` magic compare with
  `"B3D 1.1 "`.
* `FUN_004d1290` material, `FUN_004d1050` texture, `FUN_004d09b0` bone, `FUN_004db810` mesh body (strings 0x672058,
  0x6720a0 "Bone counts different - Need to re-export!!!"), `FUN_004c9b30/4c9e00` legacy vertex, `FUN_004d9bb0/4d9930`
  legacy face group, `FUN_004d0450` string.
* `FUN_00529490` (string "LEFT_HANDED" 0x671f54) turns the loaded data into engine objects and negates axes when the
  file is *not* LEFT_HANDED.
* `Gfx_G3D_Mesh::LoadMesh` (0x6782e0 "This G3D/B3D (%s) has not been re-converted!") consumes the result.

There are no chunk sizes: the file is a stream of 1-byte tokens each followed by a fixed/self-describing payload.
`str` = `u16 length` + that many bytes (Latin-1, no NUL). The file ends with token `0x01` (checked: end of file coincides).

### Top level tokens (F3.exe switch in `FUN_004de120`)

| tok | payload | meaning |
|---|---|---|
| 01 | - | EOF |
| 1c | u8 | "converted" flag (absent in 104 files = 0). 1 selects the preconverted mesh option (below) |
| 03 | f32 | scale (always 1.0) -> engine global `1.0` |
| 04 | f32 | "unit" (always 1024.0) -> engine global 1024 and 1/1024; **not** a position scale (see axes) |
| 05 | str | coordinate system, always `LEFT_HANDED` (3912/3912) |
| 16 | u32 | stored at object+0xC0 (-1 in props/tiles, 1 in some critters); meaning unknown |
| 07 | material | see below; 1 per used material |
| 08 / 09 | texture | 08 only occurs (4392x); 09 = same payload, stored with flag 1 (not seen) |
| 0e | bone | skeleton bone, see below (13740 in 'critter-like' files, also a few props/items) |
| 0a | node | nested node stream; every file: root node "Scene Root" containing exactly one child with one mesh |
| 06 | 12 bytes | 3 floats (engine flags it); not present in the demo |
| 17 | ? | `FUN_004d2620`; not present in the demo |

Typical prefix: `"B3D 1.1 " 1c 01  03 <1.0f>  04 <1024.0f>  05 "LEFT_HANDED"  16 <i32>  07 ... 08 ... [0e ...] 0a ... 01`.

### Material (tok 07)

```
str shader        // technique id: BASE_2X, BASE_VERTEX_ALPHA_2X, BASE_IMAGE_ALPHA_2X, SK_BASE_LM_2X (tiles, lightmapped),
                  // SK_BASE_LM_VERTEX_ALPHA_2X, NO_TEXTURE_VERTEX_ALPHA, BASE_LM_IMAGE_ALPHA_2X, ...  (10 distinct)
str name          // e.g. "VL_BOS_Cloth"
f32[4] x 4        // four RGBA colours (observed values: (1,1,1,1), (.5,.5,.5,1), (0,0,0,1), (0,0,0,0) in this order;
                  //  engine copies them to +0x3c..+0x78; assignment ambient/diffuse/specular/emissive is NOT proven)
f32, f32          // stored as doubles at +0x80/+0x88 (shininess-like), usually 0 and 1
str blend         // OPAQUE (7844) | ALPHABLEND (533) | ADD (16)   (checked by Gfx_G3D_Mesh: "Unknown blend_state")
str surface       // SILENT, SAND, CONCRETE, GRASS, STONE, METAL   (footstep surface; "Unknown material: %s")
u32 mask          // bit0: 8 bytes, bit1: 8, bit2: 16, bit3: 16, bit4: 24, bit5: 20 follow (observed masks 0,2,3,6,16,24)
...optional blocks per bit...
u32 flags2        // bit0..5 -> six bool flags at +0x91,+0x90,+0x92,+0x93,+0x94,+0x96
```

### Texture (tok 08)

`u8 flags (5 bits -> bools; always 3), str name, str file, u32 w, u32 h`. `name == file` in all 4392 cases, `file` is the
resource name of a TGA in the same or another grp (**4381/4392 resolve to an extracted file**; the 11 misses are missing
demo assets such as `MusMadMax_LG.tga`). `w`,`h` are the nominal size (256 for the common ones), not read back from the TGA.

### Bone (tok 0e)

`str name, str parent(always empty in the demo), u8 flags, [flags&1: f32[3] position] [flags&2: f32[4] axis(xyz)+angle(deg)]
[flags&4: f32[3]]`. Flags seen: 3 (13293), 1 (411), 0 (29), 2 (7); bit2 never occurs. Evidence: `FUN_004d09b0` = string +
virtual `Read` of a transform object; payload verified on `CR_Ant` (`Root`: pos (0,0.035,-0.010), axis (-1,0,0) angle 90;
`L ForeHip`: axis (0,.707,-.707) angle -180). Hierarchy is *not* stored in the bone token; it is implied elsewhere (GR2
skeleton) - unresolved.

### Node stream (tok 0a ... 0b)

Tokens inside: `0c str` node name, `0a` child node (recursive), `0f` mesh, `1b` (`u32 n, n x str` - bone-group names such as
"Chest","UpperArms","Legs","Waist","ForeArms","Shins" on 88 critter armour pieces), `0b` end of node.
Observed in every file: `0a 0c"Scene Root" 0a 0c<meshname> 0f<mesh> [1b..] 0b 0b`. No per-node transform exists.

### Mesh (tok 0f)

```
str name           // always empty
if (converted-flag) u8 hasData  else hasData = 0
```
Two layouts for the mesh body:

**A. preconverted (hasData=1; 881 meshes, all critter/prop variants)** - GPU-ready, vertex stride always 44:
```
u32 vertexCount, u32 stride(=44)
byte vertices[count*stride]   // f32 pos[3]; f32 normal[3]; u8 b,g,r,a (D3DCOLOR); f32 uv0[2]; f32 uv1[2]
f32 bboxMin[3], f32 bboxMax[3]
u8  hasLightmap ; if 1: u32 4; then 4 sets x vertexCount x (f32,f32)  (511 meshes)
u32 G                          // groups (materials)
G x group { str name; str shader; f32[4]x4 colours; f32; str blend; str surface;
            u8,u8,u8; u32; u8; u32; u8; u32,u32; u8; u32,u32; u8; u32,u32,u32; u8; u32 x5(last = textureCount k);
            if k>=1 str diffuseTexture; if k>=2 str secondTexture; u32 n; n x str }
G x str            // group names again
u32 indexCount[G]; u32 triCount[G] (= indexCount/3 in all samples)
per group: u16 indices[indexCount]   // absolute vertex indices, triangle list
```
(F3.exe errors "Bad VertexSize for preconverted mesh" if stride != 44.) The 44-byte vertex = pos(12)+nrm(12)+colour(4)+2 UV(16).
bbox of the signpost `(-1.098,-0.015,-0.120)-(1.098,7.704,0.060)` matches the vertex data.

**B. legacy list (hasData=0 or flag absent; 3031 meshes - all Tiles, most Props)**:
```
u32 vertexCount
vertex { f32 pos[3]; f32 normal[3]; f32 rgba[4] (float colour, usually 0.5/0.5/0.5/1 or vertex-alpha);
         u32 uvCount; f32 uv[uvCount][2];   // counts seen: 2 (826k), 1 (101k), 0 (20k): uv0 = diffuse, uv1 = lightmap
         u32 weightCount; { u32 boneIndex; f32 weight } x weightCount }   // 0..2 per vertex
u32 groupCount
group { u32 materialIndex (into tok-07 list); str name; u32 n; n x str (n==1: texture file e.g. "Ext5x5_City2.tga");
        u32 m; m x str (always 0); u8 primitive (always 0xC9 = triangle list);
        u32 indexCount; u32 index[indexCount]; u8 flag[indexCount] (always 1) }
```
Evidence: `FUN_004c9b30` (vertex ctor), `FUN_004c9e00` (pos/normal via `FUN_004c9400` reading 3 floats each widened to double, rgba via
`FUN_004c89e0`, UV sets via `FUN_004c9380`, weights `FUN_004c8c20` = `u32,f32`), `FUN_004d9bb0` (`u8 - 0xC9` primitive, u32 n, n x u32,
n x u8). Every index list length is a multiple of 3 and every index is < vertexCount (checked on all 6944 groups).

### Axes, handedness, scale

* All files say `LEFT_HANDED`; F3.exe negates coordinates for any other value, i.e. the engine is left-handed (Direct3D).
* **Y is up.** Evidence: signpost bbox y in [-0.015, 7.70] with z in [-0.12, 0.06]; tiles are flat in Y (`RoadMesa RoadDirt_1`:
  y = -0.10 exactly, floors y in [-0.01,0.01]) and span x,z. Car (`DS_copcar`): x [-0.36,0.36], y [0,0.67], z [-1.04,0.93].
* Triangles are wound so that `cross(v1-v0, v2-v0)` points along the vertex normal when the raw numbers are read as right-handed
  maths (19344 of 19360 tiled triangles tested) - i.e. clockwise-front in the engine's left-handed view (D3D default CW culling).
* Units: floor tiles occupy the **footprint x,z in [0,0.5] for `Ext5x5_*` and [0,1.0] for `Int10x10_*`/`Ext_*_10x10`** with the
  origin at the tile corner (checked on 6 tiles). So one tile "5x5" == 0.5 file units (1 unit = 10 tile cells); the tokens 03/04
  (1.0/1024.0) do not rescale positions. Absolute prop size (e.g. a 0.67-unit tall car) is therefore smaller than the tile grid
  suggests; per-instance scale lives in the map/entity data (not reversed).
* UV origin is Direct3D style (v down). `modelinfo --obj` therefore writes `v = 1 - v`.
* `modelinfo --obj` default conversion: z negated, face winding reversed (-> right-handed, Y up, CCW). `--raw` keeps the file values.
  Output: one `g`/`usemtl` per group; `.mtl` has `map_Kd <tga resource name>`; lightmap UV sets and weights are not exported.

Stats over the 3912 parsable files: converted flag 1 = 3808, absent = 104; exactly 1 mesh per file; textures: tok 08 flags 3.

## 2. TGA

1614 TGA resources + 39 BMP. All plain TGA 2.0 (no colour map, no palette anywhere):

| type | depth | origin | count |
|---|---|---|---|
| 2 (raw truecolour) | 24 bit | bottom-left | 1129 |
| 2 | 24 | top-left (desc bit5) | 58 |
| 2 | 32 (8 alpha bits) | bottom-left | 339 |
| 10 (RLE) | 24 | bottom-left | 60 |
| 10 (RLE) | 32 | bottom-left | 25 |
| 2, 24 bit with image-id string | | | 3 |

Sizes: 256x256 (626), 64x64 (402), 32x32 (188), 128x128 (82), 16x16 (70), 128x64 (53), 512x512 (25) ... 1043 files end with a
`TRUEVISION-XFILE` footer. Alpha is real only in the 32-bit files (e.g. `*_LG.tga` critter textures, `ALPHABLEND` shader materials);
24-bit textures with blend `ALPHABLEND` rely on vertex alpha (`BASE_VERTEX_ALPHA_2X`). A decoder must honour the descriptor origin bit
and RLE; `modelinfo tga --png` does both.

How b3d refers to textures: group (preconverted: `textures[0]`, legacy: the single string in the group) and the top-level tok 08
list both name the **resource name incl. `.tga`** (case-insensitive), e.g. `Ext5x5_City2.tga`, `PS_BOS.tga`. 7356/7368 group references
resolve to an existing TGA resource. Shader technique names (`BASE_2X`, `SK_BASE_LM_2X`, ...) select blending/lightmap use:
`SK_BASE_LM_*` = diffuse+lightmap (uv1 / the 4 lightmap sets), `*_VERTEX_ALPHA` = alpha from vertex colour, `*_IMAGE_ALPHA` = alpha from texture.
(Interpretation from the names plus the observed data; the pixel pipeline itself was not traced.)

## 3. Granny2 (`.mesh.gr2`, `.anim.gr2`)

`granny2.dll` (230 KB, 2003, exports `_GrannyReadEntireFile@4`, `_GrannyGetFileInfo@4`, `_GrannyDecompressData@28`, ...) is shipped in the
game dir. All 547 files are Granny file format **version 6**, little endian 32-bit (magic `b8 67 b0 ca f8 6d b1 0f 84 72 8c 7e 5e 19 00 1e`).

```
0x00 u8[16] magic
0x10 u32 headerSize (=352 = 0x160, includes the section/relocation tables)   0x14 u32 headerFormat (0)   0x18 u32[2] reserved
0x20 u32 version (6)  u32 fileSize (== real size, all files)  u32 crc  u32 sectionArrayOffset (0x38, relative to 0x20)  u32 sectionCount (6)
0x34 u32 rootTypeSection,rootTypeOffset  u32 rootNodeSection,rootNodeOffset  u32 tag (0x8000000f)  u32 extra[4]
0x58 section headers, 44 bytes each:
     u32 compression (0 none, 1 Oodle0, 2 Oodle1), u32 dataOffset, u32 compressedSize, u32 decompressedSize, u32 alignment,
     u32 first16bit, u32 first8bit,  u32 relocOffset, u32 relocCount, u32 marshalOffset, u32 marshalCount
relocations: 12 bytes each {u32 offsetInSection, u32 targetSection, u32 targetOffset} located right before the section data
```
(Check: for `CR_Ant.mesh.gr2` relocations 246 x 12 = 2952; 352 + 2952 = 3304 = data offset of section 0.)

**Compression: all 547 files have section 0 stored with compression id 1 (Oodle0, Granny's proprietary arithmetic coder), sections 1-4
are empty (size 0) and section 5 is uncompressed/empty.** e.g. `CR_Ant.mesh.gr2`: 5276 -> 13868 bytes; `CR_AntQ_Com_Una_Attack1_01.anim.gr2`:
29828 -> 48788. So the geometry/skeleton/animation payload cannot be read without an Oodle0 decoder.

Decision: container (header, sections, compression, relocation counts) is parsed natively (100 %); the payload is currently decoded by
dynamically loading the original `granny2.dll` (must be a **32-bit process**; `modelinfo` built with `-A Win32`). A native Oodle0 decoder
is the remaining work to remove the DLL dependency (unresolved, see below).

Object counts (root `granny_file_info`, counts at byte offsets 16 + 8*k after three leading pointers, order Textures, Materials, Skeletons,
VertexDatas, TriTopologies, Meshes, Models, TrackGroups, Animations; verified by plausible results on all 547 files):

| files | content |
|---|---|
| 405 + 39 + 2 + 2 | animation-only files: TrackGroups 1..4, Animations 1 = **448** (all `*.anim.gr2`) |
| 68 + 3 | skeleton only: Skeletons 1..3 + same number of Models, **no Meshes** |
| 2 + 1 | skeleton(s) plus a track group + animation (`.mesh.gr2` that also carries an animation) |
| 25 | full model: Skeletons 1, Models 1, VertexDatas 1, TriTopologies 1, Meshes 1 |

So the 99 `.mesh.gr2` split into 71 skeleton-only, 3 skeleton+animation, 25 with real mesh data.

Textures/Materials are always 0 in GR2 (materials come from the matching b3d / ini). The animated critters therefore combine: the `.b3d`
(bone list token 0e, mesh with 0..2 bone weights/vertex, materials and textures) and the GR2 skeleton/animation.

## 4. Unresolved / not done

* Native Oodle0 decompressor (needed to parse GR2 payload without `granny2.dll`); GR2 type-tree walk (skeleton bones, track curves) is
  therefore not implemented; counts come from the DLL.
* b3d: exact meaning of material colour slots, `mask` blocks, `flags2`, tok 16, the 3 small `u8` flags + u32 fields in the preconverted
  group record, tok 06/09/17 (absent in the demo), and the bone hierarchy (parent string is empty; hierarchy presumably from GR2).
* The 3 files with token numbering `35 01 ...` (`CR_Badger`, `CR_Bear`, `CR_Sheep`) and the text `.g3d` (`CR_Monsta`).
* Per-instance prop/tile scale and rotation (map files `TRE`/`EMAP`/`mapdata` are not reversed); why 5x5 tile = 0.5 units.
* Winding/handedness conclusions come from normals-vs-winding statistics and the LEFT_HANDED token handling, not from tracing the D3D
  state setup. Pixel pipeline for the `SK_BASE_LM_*` techniques was not traced.

## Skin weights in preconverted critter meshes (finding)

In meshes with the converted flag (vertex stride 44) the block read as "4 lightmap uv sets" is 4 skin influences per
vertex, `{u32 boneIndex (0xFFFFFFFF = unused), f32 weight}`, indexing the tok 0e bone list. Bone names match the Granny
skeleton; b3d vertex units are exactly 10x Granny units. Skin matrix (row vectors) = `inverseWorld[j] * poseWorld[j]`.
