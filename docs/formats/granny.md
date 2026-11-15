# Granny 2 (`.gr2`) format and the native `gr2lib` reader

Scope: the 547 `.gr2` files of the tech demo (`*.mesh.gr2` = skeleton/model, `*.anim.gr2` = animation). All are Granny **file format
version 6**, little endian, 32-bit pointers, written by "Granny Standard Exporter, SDK version 2.2.0.0" (read by the shipped
`granny2.dll`, 230 KB). Implementation: `tools/gr2lib` (dependency-free C++17). Everything below was derived from the files plus the
decompilation of `granny2.dll` (addresses are RVAs+0x10000000 as shown by the analysis tool) and is **verified against the DLL itself**
(section 5).

Tools: `gr2dump` (summary / `--json` / `--tree` / `--sample` / `--scan`), `gr2verify` (native decoder and pose sampler vs the DLL,
needs a `-A Win32` build and the original `granny2.dll`, which is never committed).

```
cmake -S tools/gr2lib -B build/gr2lib                  && cmake --build build/gr2lib --config Release
cmake -S tools/gr2lib -B build/gr2lib32 -A Win32       && cmake --build build/gr2lib32 --config Release   # for gr2verify --dll
gr2dump  extracted/Critters/CR_AntQ_Com_Una_Walk_01.anim.gr2 --json walk.json --fps 30
gr2dump  --scan extracted                              # success rates / curve statistics over a tree
gr2verify extracted --dll F:/_work/F3_Demo/granny2.dll # Oodle0 byte-exactness vs the DLL
gr2verify --pose-all extracted --dll F:/_work/F3_Demo/granny2.dll   # sampled bone transforms vs the DLL's own sampler
```

## 1. Container

```
0x00 u8[16] magic b8 67 b0 ca f8 6d b1 0f 84 72 8c 7e 5e 19 00 1e
0x10 u32 headerSize (0x160)  u32 headerFormat(0)  u32[2] reserved
0x20 u32 version (6)  u32 fileSize  u32 crc  u32 sectionArrayOffset (0x38, relative to 0x20 => section headers at 0x58)
0x30 u32 sectionCount (6)  u32 rootTypeSection, rootTypeOffset  u32 rootObjectSection, rootObjectOffset  u32 tag (0x8000000f)  u32[4]
section header, 44 bytes: u32 compression (0 none, 1 Oodle0, 2 Oodle1), dataOffset, compressedSize, decompressedSize, alignment,
    first16bit, first8bit, relocOffset, relocCount, marshallOffset, marshallCount
relocation, 12 bytes at relocOffset: u32 offsetInSection, u32 targetSection, u32 targetOffset
```
Loading = decompress every section, lay all sections out in one flat buffer, and for every relocation write
`base[targetSection] + targetOffset` at `base[section] + offset`; afterwards all pointers are offsets into that buffer (`File::mem`,
offset 0 = null) and the file behaves like a 32-bit address space. The marshalling tables (byte swapping) are not needed for
little-endian files.

All 547 files: section 0 is Oodle0 (id 1) and holds the type definitions, string data and all objects. 25 files with vertex data
also have compressed sections 3 and 4 (597 compressed sections in total, all decoded). `first16bit`/`first8bit` are the Oodle0
"stop" offsets (section 3.1).

## 2. Oodle0 (compression id 1)

`GrannyDecompressData(format, byteReversed, compressedBytes, stop0, stop1, stop2=decompressedSize, out)` (`0x100274d0` ->
`0x100140d0`) dispatches format 1 to `0x1001d060`. Native port: `tools/gr2lib/oodle0.cpp`.

Stream: 9 header words `{w0,w1,w2}` x 3 passes, then an arithmetic-coded bit stream (LSB-first bits of little-endian 32-bit
words; the first word contributes bit 31 to the bit buffer and bits 0..30, bit 0 = MSB, as the initial 31-bit `code`).
Three passes decode up to output offset `stop0`, `stop1`, `size` respectively, each with freshly built models taken from its header
words (the coder state is carried over). Per pass:

| field | meaning |
|---|---|
| `w0 & 0x1ff` | literal alphabet size (escape range for new literals) |
| `w0 >> 9` = A | window parameter: `B = min(A,4)` = size of the "distance low" alphabet; escape range for the distance high part is `(min(A, bytesProducedInPass) >> 2) + 1` |
| `w1 & 0x1ff` | capacity of the literal model |
| `w1 >> 9` | capacity of the "distance high" model |
| `w2` | 4 bytes = capacities of the 4 x 16 context models (context = previous code 0..63); context 64 uses `w2 & 0xff` |

Chunk decode (`0x1001cbe0`): code = decode(model[ctx]) where ctx = previous code (0 initially); a code is a symbol 0..64
(escape -> uniform over 65). Code 0: literal byte from the literal model. Code c>0: match length `c+1` for c < 0x3d, else
`{0x80,0xc0,0x100,0x200}[c-0x3d]` (table at `0x10033cf8`); `lo` from the B-model, `hi` from the distance model, distance
`hi*4 + lo + 1`, bytes copied forward (overlap allowed).

Models (`0x1000e0c0`, `0x1000e6f0`, `0x1000e7f0`, `0x1000ec30`, `0x1000e690`, `0x1000e130`, `0x1000e1c0`): adaptive
frequency models over *ranks* with a 16-group cumulative-count tree (group shift/size chosen by `0x1000e130`), rank 0 = escape
(initial frequency 3), new symbols appended on escape (frequency 2, symbol coded uniformly), increment 1 per hit, rescale (halve,
drop entries < 2, move the largest to the first slot of its group) when the total exceeds 0x3fff. Range coder (`0x1000ddc0`): 31-bit
`low/high/code`, `(range*cum)/total` interval update, renormalise while the top bits agree, E3-style underflow handling.

Verification: `gr2verify extracted --dll granny2.dll` decompresses every compressed section natively and through
`_GrannyDecompressData@28`: **597 sections in 547 files byte-identical, 0 mismatches**; no file fails to decode. Whole library scan
(~1.8 s for all files, no optimisation work done).

## 3. Type system

The root type pointer addresses an array of 32-byte member definitions
`{i32 type, char* name, def* referenceType, i32 arrayWidth, i32 extra[3], i32 ignored}` terminated by type 0. Member types
(same numbering as `granny.h`): 0 End, 1 Inline, 2 Reference (ptr), 3 ReferenceToArray (count, ptr), 4 ArrayOfReferences (count, ptr to
pointers), 5 VariantReference (type ptr, object ptr), 7 ReferenceToVariantArray (type ptr, count, ptr), 8 String (ptr), 9 Transform
(68 bytes), 10 Real32, 11 Int8, 12 UInt8, 13 BinormalInt8, 14 NormalUInt8, 15 Int16, 16 UInt16, 17 BinormalInt16, 18 NormalUInt16,
19 Int32, 20 UInt32, 21 Real16, 22 EmptyReference; primitive/Inline members repeat `max(arrayWidth,1)` times; members are packed
without padding. `Walker` (gr2_types.cpp) reads any object into a generic `Value` tree (`gr2dump --tree`). Verified on all files:
every root object resolves, no pointer falls outside the flat buffer.

`granny_transform` (68 bytes): `u32 flags; f32 position[3]; f32 orientation[4] (x y z w); f32 scaleShear[9]` (3x3, row major).

Root object (88 bytes): `_GrannyFileStringTable*` (a pseudo member holding `StringOffsets[]`/`Characters[]`),
`ArtToolInfo*` (`FromArtToolName`, revisions, `UnitsPerMeter`, `Origin/RightVector/UpVector/BackVector`: LightWave 7.5 for the files
seen), `ExporterInfo*`, `FromFileName` (string), then nine `ArrayOfReferences`: Textures (0 everywhere), Materials (0),
Skeletons, VertexDatas, TriTopologies, Meshes, Models, TrackGroups, Animations (offsets 16, 24 ... 80; this is the layout the DLL's
`GrannyGetFileInfo` returns, used by the verifier).

* Skeleton: `Name`, `Bones[]` (156 bytes): `Name, ParentIndex (i32, -1 = root), Transform, InverseWorldTransform (f32[16]),
  LightInfo*, CameraInfo*, ExtendedData (variant)`. Parents always precede children in the data (0 violations in 1176 bones).
  The 4x4 convention is row-vector (`v' = v*M`, translation in the last row). Check performed on every bone: rest-pose
  world matrix (`world = local * parentWorld`, `local = ScaleShear * Rot(q)^T`, translation row) times the stored inverse world gives
  the identity (max error 0.012, all 1174 real bones; only the two non-skeletal helper bones `Camera` and `Light` differ).
* Model: `Name, Skeleton*, InitialPlacement, MeshBindings[] -> Mesh*`.
* Mesh: `Name, PrimaryVertexData*, MorphTargets[], PrimaryTopology*, MaterialBindings[] (Material* always null), BoneBindings[]
  {BoneName, OBBMin, OBBMax, TriangleIndices[]}, ExtendedData`. VertexData `Vertices` is a ReferenceToVariantArray, i.e. the vertex
  struct is described in the file (seen: `Position[3], BoneWeights[4] (NormalUInt8), BoneIndices[4] (UInt8), Normal[3],
  TextureCoordinates0[2]`; the door meshes use 2 bone indices). TriTopology: `Groups[] {MaterialIndex, TriFirst, TriCount}`,
  `Indices` (Int32) or `Indices16`, plus unused maps. 25 files carry meshes: 8694 vertices / 4372 triangles in total, every index in
  range, all bone weights sum to 1. These meshes are the low-poly collision/proxy geometry; the renderable models are in the `.b3d`
  files (see models.md). **6 meshes (door props: `DS_City1_Doors*`, `DS_junktown1*JT_DoorL`, `DS_Vault13_Doors*Elevator_door01`)
  store an uninitialised topology (all group fields and index values are `0xBAADF00D`)**; the loader flags these with
  `Mesh::topologyValid = false` and returns no indices.
* TrackGroup (124 bytes): `Name, ScalarTracks[] {Name, ValueCurve}, TransformTracks[] {Name, PositionCurve, OrientationCurve,
  ScaleShearCurve}, TextTracks[], InitialPlacement, AccumulationFlags (i32: 0 in 525 groups, 1 in 12), LoopTranslation[3] (always 0),
  PeriodicLoop*, ExtendedData`. Scalar tracks and text tracks do not occur in the shipped files.
* Animation: `Name` (the source `.lws` scene path), `Duration (f32)`, `TimeStep (f32, 1/30)`, `TrackGroups[]` (array of references,
  the same objects as the file-level `TrackGroups`). A track group's `Name` is the model/object it was exported from (the DLL binds an animation to a
  model by comparing track group name and model name, which often differs, e.g. group `CR_Bat` vs model `CR_Bat_Coll`); track names
  are bone names. Some animation files hold extra groups `Light` and
  `Camera` (scene objects of the LightWave export).

## 4. Curves

There are **no variant curve formats** (`DaK32fC32f`, `DaIdentity`, `D3Constant32f`, ... belong to later Granny versions): every curve
is the Granny 2.2 inline struct

```
struct curve { i32 Degree; f32 Knots[count, ptr]; f32 Controls[count, ptr]; }   // 20 bytes
```
Statistics over all 12331 transform tracks (`gr2dump --scan`): **every curve has `Degree = 0`**; there are no degree 1/2/3 curves.

| curve | dim | animated (>1 knot) | constant (1 knot) | absent (0 knots) |
|---|---|---|---|---|
| position | 3 | 416 | 11450 | 465 |
| orientation | 4 | 8863 | 2904 | 564 |
| scale/shear | 9 | 14 | 7 | 12310 |

`Controls.count == Knots.count * dim` for all 23654 non-empty curves. Animated curves are dense samples: of 9293 curves with >= 2
knots, 8890 have uniformly spaced knots (spacing = Duration/(n-1), mostly 20..40 keys per second, e.g. 32, 21, 24, 30 per second;
the 403 non-uniform ones are the exception) and in all of them the last knot equals the animation `Duration`. Knot times are seconds
from the start of the animation.

Sampling (`gr2::Curve::sample`, mirrors `FUN_1000fef0`, `0x1000adc0`, `0x1000aeb0`, `0x1000ad10` and the `SampleBSpline` kernels):
one knot -> constant `Controls[0..dim)`; otherwise clamp `t` to `[Knots[0], Knots[n-1]]`, find `i` = first knot >= t clamped to
`[1, n-1]` and evaluate degree d at `ti = &Knots[i]`, `pi = &Controls[i*dim]`:

* degree 0: `result = pi[0]`, i.e. **the control of the knot at or after t** (so at t = 0 the value of key 1 is returned, not key 0);
* degree 1: `u = (t-ti[-1])/(ti[0]-ti[-1]); u*pi[0] + (1-u)*pi[-1]`, quaternions then scaled by `(3-|q|^2)/2` (`SampleBSpline1x4n`);
* degree 2: quadratic B-spline kernel (`0x1000a2a0`), end knots/controls replicated at the borders.

Degrees 1/2 are implemented from the decompilation but are **never exercised by the shipped data**; degree 3 is not implemented
(`sample` returns false). A track missing a curve leaves the bone's rest value (`Bone::local`) in place: checked against the DLL.
Orientation controls are *not* all unit length (e.g. `CR_AntQ_Com_Una_DmgFr_01`: |q| about 0.98 at some keys; the DLL returns them
unchanged for degree 0), renormalise before use.

## 5. Verification summary

| check | result |
|---|---|
| container + Oodle0 + relocations + type walk + scene extraction | 547 / 547 files |
| Oodle0 vs `_GrannyDecompressData@28` | 597 / 597 compressed sections byte-identical |
| sampled local pose vs the DLL (`GrannyInstantiateModel`, `GrannyPlayControlledAnimation`, `GrannySetControlRawLocalClock`, `GrannySampleModelAnimations`, `GrannyGetLocalPoseTransform`) | 448 / 448 animation files, 9 sample times (0 .. duration) x all bones of a covering skeleton, max abs difference 1.4e-6 over position, orientation (no sign flips) and scale/shear |
| bones | 99 skeleton files, 1176 bones, 0 invalid parents, 1174 consistent with their inverse world matrix |
| meshes | 25 files, 0 out-of-range indices (6 uninitialised topologies flagged), 0 bone-weight sums != 1 |

Oracle notes: the DLL binds an animation by track group name = model name, so `gr2verify` overwrites the in-memory model name with the
name of the covering track group; skeletons are chosen per animation file by bone-name coverage (prefix match first). 448 animation
files are checked against 99 skeleton files this way.

## 6. Library API (`tools/gr2lib/gr2.h`, namespace `gr2`)

```cpp
bool oodle0Decompress(const uint8_t* src, size_t srcSize, uint32_t stop0, uint32_t stop1, uint32_t outSize, uint8_t* out);

class File { bool load(const uint8_t*, size_t, std::string* err); bool loadFile(path, err);
             sections, mem (flat relocated memory), rootType, rootObject; u32/u16/u8/f32/cstr(off);
             bool decompressSection(int idx, std::vector<uint8_t>&) const;   // un-relocated, DLL-comparable
};
class Walker { Walker(const File&); const TypeDef& type(off); Value read(typeOff, objOff); Value readRoot(); };  // generic tree

struct Transform{flags, position[3], orientation[4] xyzw, scaleShear[9]};      struct Bone{name, parent, local, inverseWorld[16]};
struct Skeleton{name, bones};  struct Curve{dim, degree, knots, controls; bool sample(float t, float* out) const;};
struct TransformTrack{name, position, orientation, scaleShear};  struct TrackGroup{name, transformTracks, vectorTracks, initialPlacement,
                         loopTranslation, flags};  struct Animation{name, duration, timeStep, trackGroups};
struct Mesh{name, vertexCount, indexCount, positions, normals, uv0, boneWeights, boneIndices, groups, indices, boundBones, topologyValid};
struct Model{name, skeleton, initialPlacement, meshes};  struct Scene{fromFileName, skeletons, animations, trackGroups, meshes, models};

bool loadScene(const File&, Scene&, std::string* err);                          // extraction from the generic tree
Transform sampleTrack(const TransformTrack&, float t, const Transform& rest);    // missing curves keep `rest`
std::vector<Transform> samplePose(const Skeleton&, const TrackGroup&, float t);  // per-bone local pose at time t
void transformToMatrix(const Transform&, float m[16]);                           // row-vector 4x4 (Granny)
std::vector<float> worldMatrices(const Skeleton&, const std::vector<Transform>* pose = nullptr);
std::string sceneToJson(const Scene&, float fps = 0, bool includeSamples = true);
```
JSON (`gr2dump file --json out.json [--fps N] [--no-samples]`): skeletons (bones with parent, local transform, inverse world), mesh
summaries, animations with per-track `{t, p, q, s}` samples at `fps` (default `1/TimeStep` = 30) from 0 to `Duration`.

## 7. Unresolved / not done

* Curve degrees 1..3 and variant curve formats never occur in the demo data; degree 1/2 are untested against the DLL, degree 3 is missing.
* Oodle1 (compression id 2) and big-endian / byte-reversed files are not supported (none present).
* `AccumulationFlags = 1` (12 groups, all with `LoopTranslation = 0`) and `PeriodicLoop` (always null) are parsed but their runtime
  semantics (root-motion extraction) were not traced; the DLL's `GrannyGetTrackGroupInitialPlacement4x4` / accumulation functions were not
  analysed. `InitialPlacement` is the identity in every file.
* Vertex data is exposed for the layouts seen (`Position`, `Normal`, `BoneWeights`, `BoneIndices`, `TextureCoordinates0`); other vertex
  members are only reachable through the generic `Walker`.
* The `.b3d` <-> `.gr2` bone matching (names) is only checked implicitly through the DLL pose comparison.

## Human animation set and weapon mapping (client: `WeaponAnim.h`, `RigLibrary::GetFor`)

`HumM_` / `HumF_` ship 93 clips each. Name = `<prefix>_<stance>_<set>_<action>_<nn>.anim.gr2`:

* stance: `Com` (combat) / `Def` (relaxed, out of combat) / `ProneB`, `ProneF` (lying down);
* set: `Una` unarmed, `1H` one-handed weapon (pistols, clubs, knives), `2HH` two-handed (rifles, heavy), `2HC` (only `Melee_Slash`);
* actions: `Idle`, `Walk` (`Com_*_Walk`; **`Run` exists only as `Com_Una_Run`**), `Gun_Shoot`, `Gun_Burst`, `Gun_NoRecoil`, `Gun_Sweep`, `Gun_Butt`,
  `Melee_Slash`, `Melee_Thrust`, `Throw` (1H only), `DmgFr` / `DmgFrMult` (hit reactions), `Dodge`, `Equip` / `Unequip`, `Switch_Weapons`, `Stunned`,
  `Knock_ProneB`, `Laydown_ProneB` (death fall), `ProneB_Una_Death`, `Fidget` (Def idles), `to_Def` / `to_Com`, `Pickup`, armor / helmet equip.

Weapon prototypes do not name their animation set (and 35 of 51 have no decoded attack data), so the client derives it from the prototype key:

| weapon | kind / set | attack clip |
|---|---|---|
| grenades, dynamite, rock, flare, throwing knife | Throw / 1H | `Com_1H_Throw` |
| knives, shiv, switchblade | Melee / 1H | `Com_1H_Melee_Thrust` |
| bat, crowbar, hatchet, wrench, pipe, nightstick, table leg, prod, machete | Melee / 1H | `Com_1H_Melee_Slash` |
| minigun, flamethrower, railgun, array gun, rifles, shotguns | Gun / 2HH | `Com_2HH_Gun_Shoot` (`Gun_Burst` when the first attack mode is a burst) |
| everything else (pistols, revolvers, SMGs, lasers, welder, hand flamer) | Gun / 1H | `Com_1H_Gun_Shoot` (`Gun_Burst` for bursts) |
| nothing in hand | Una | `Com_Una_Punch_Jab`, fallback `Kick_Snap` |

Other clips: idle `Def_<set>_Idle` (relaxed) or `Com_<set>_Idle` for 8 s after a hit / attack; walk `Com_<set>_Walk`; run `Com_Una_Run` for everyone (no weapon run
clips); hit `Com_<set>_DmgFr`; death `Com_<set>_Laydown_ProneB`; draw / holster one-shot `Com_<set>_Equip` / `Com_Una_Unequip` when the hand-1 weapon changes. Every
lookup falls back to the unarmed clip and finally to the old fixed list, so a missing clip never breaks a critter. Non-human models keep the `Una` list.
Attack timing: `CombatEvent.durMs` (the server's attack action length, `clamp(cost * AttackMsPerCost, min, max)`) is sent with every shot and the attack clip is
played at `clip duration / durMs` (clamped 0.5..2.5) so the animation spans the action.

## Weapon in hand

The active (hand-1) weapon's `GD_WP_*.b3d` (the prototype `model`, same file as the ground model, preconverted, ~1.9 units for a 9mm) is drawn on the skeleton bone
`R Handle_1` (child of `R Hand`) with the bone's animated world matrix (Granny row-vector matrix; the renderer reflects z like the skinned vertices), scale 0.5 and no extra
rotation (verified with the 9mm autoloader: the muzzle follows the forearm in the shoot clip). The transform can be tweaked without rebuilding with the env var
`F3_HELD="scale,rx,ry,rz,ox,oy,oz"`. Melee / thrown weapon models are drawn the same way and were not individually checked.
