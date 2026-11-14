# Van Buren tech demo (F3.exe) - container formats

All integers little-endian u32. Evidence: byte-level analysis of the shipped data
(`F:\_work\F3_Demo`) plus the loader in F3.exe (`FUN_0049bf20`, "ResourceSystem::Init",
which references the string `resource.rht` at 0x66abc8; xref from 0x49c040). `tools/grptool`
implements everything below and was run over the full demo (7044 resources, 0 failures).

Nothing is compressed or encrypted: entries are stored raw in the .grp files.

## 1. `data\*.grp` - raw blob archives (NO names)

```
u32 version  = 2
u32 one      = 1
u32 count
struct { u32 offset; u32 size; } entries[count]   // offset is absolute file offset
<data>
```

* Offsets are contiguous: `entries[0].offset == 12 + 8*count`, `entries[i+1].offset == entries[i].offset + entries[i].size`,
  and the last entry ends exactly at EOF (verified for all 18 non-empty grps).
* Size 0 entries exist (2 of them).
* Empty grps (12 byte file, count 0): Templates, _CHM, _Dev, _ESF, _MSC, _RLZ.
* `data\Demo\data\Sounds.grp` is byte-identical to `data\Sounds.grp`.
* Names, types and grouping come only from `resource.rht`.

Files and entry counts: Tiles 2979, Critters 1574, Interface 453, Maps 668, Props 437, Items 226,
Effects 147, Sounds 129, Scripts 121, Engine 25, _CRT 108, _WEA 51, _CON 48, _ARM 22, _AMO 20, _USE 18,
_DOR 13, _ITM 5. Counts equal the number of rht records per grp exactly (sum 7044).

## 2. `resource.rht` - resource hashtable / name table

Loader (F3.exe `ResourceSystem::Init`): reads whole file; fails with "Hashtable file wrong version"
unless `w[0]==1 && w[1]==1`; loops `w[2]` records starting at byte 20; for each record builds two C strings
(`base + w[4] + rec.nameOff`, `base + w[3] + rec.grpNameOff`) and calls
`FUN_0050bf30(grpName, type, flag, resName, index)` (inserts into the resource hashtable;
ini `[Resources] resource_hashtable_size=0x1000, temp_hashtable_size=0x200, cache_size=1000000`).

```
u32 version = 1
u32 version2 = 1
u32 count        = 0x1b84 (7044)
u32 grpNameBase  = 0x22664   // file offset of grp-name string table
u32 nameBase     = 0x226f7   // file offset of resource-name string table
struct Record (20 bytes) x count, starting at 0x14:
    u32 flag      = 1 (always)
    u32 index     // entry index inside the grp file
    u32 type      // resource type id (100..3700, see below)
    u32 nameOff   // offset (from nameBase) of NUL-terminated resource name
    u32 grpOff    // offset (from grpNameBase) of NUL-terminated grp base name ("Tiles", ...)
// 0x22664: "Tiles\0Interface\0Critters\0Engine\0Templates\0Scripts\0Maps\0Effects\0_Dev\0Props\0Sounds\0Items\0_CON\0_CHM\0_ARM\0_AMO\0_CRT\0_DOR\0_ESF\0_ITM\0_MSC\0_RLZ\0_USE\0_WEA\0"
// 0x226f7: resource names, back to back, to EOF
```

So `grp = data\<grpName>.grp`, entry = `grp.entries[index]`. (grp, index) is unique (every entry named exactly once);
the same *name* can appear several times with different types (e.g. map `Mainmenu` as `tre`, `emap`, `mapdata`), and different
grps (e.g. `footsteps`). Names are case-preserved, contain no path separators, and some look like `Int10x10_Cave1_Corner{1}Cave Corner1 (0)`
(Tiles models named `<set>{<n>}<mesh>`). Name uniqueness key is (grp, name, type). Records are NOT sorted.

### Resource type ids (observed by magic bytes / content)

| type | grp(s) | count | magic / content | grptool ext |
|---|---|---|---|---|
| 100 | Tiles, Critters, Props, Items, Effects | 3916 | `B3D 1.1 ` (3915; one is text `# G3D File` = CR_Monsta) static/skinned mesh | `.b3d` (`.g3d`) |
| 200 | all gfx grps | 1653 | TGA (type 2/10, may have image-id string, 1614) or BMP `BM` (39) | `.tga` / `.bmp` |
| 300 | Scripts | 36 | compiled script, first u32 == entry size, then `e0 f1 06 06 02 00 28 00` | `.scra` |
| 400 | Engine, Maps | 39 | `8TRE` + u32 size, chunks (`HEAD`...) - map/tile engine data | `.tre` |
| 600 | Critters | 99 | Granny2 (`b8 67 b0 ca f8 6d b1 0f`), skeleton/mesh | `.mesh.gr2` |
| 700 | Critters | 448 | Granny2, animations (`*_Com_Una_*_01`) | `.anim.gr2` |
| 800 | Critters, Interface, Engine, Scripts | 125 | text ini (`[ANIM_CUES]`, `[Audio]`, `;---`, ...) | `.ini` |
| 1100 | Sounds | 113 | `RIFF` WAV | `.wav` |
| 1200 | Scripts | 83 | compiled script (same header as 300) | `.scrb` |
| 1300 | Engine, Maps | 38 | `EMAP` + u32 5 | `.emap` |
| 1400 | _CRT | 108 | `EEN2` entity template (critters) | `.een` |
| 1500 | Effects | 119 | `VEG V1.1` effect | `.veg` |
| 1600 | Interface | 25 | `GUI V1.2` layout | `.gui` |
| 1700 | Maps, Engine | 39 | map data, starts `a4 00 00 00 86 00 00 00 ...` (w,h-like u32s) | `.mapdata` |
| 1800/2400/2500/2600/2700/3500/3600 | _ITM/_WEA/_ARM/_DOR/_USE/_AMO/_CON | 5/51/22/13/18/20/48 | `EEN2` entity templates | `.een` |
| 1900-2300 | Sounds | 19+3+1+1+1 | text "// FO3 Sound File" sound scripts | `.sndscript` |
| 3700 | Engine | 1 | text `DEVILDOG=1986` (string-id token list into English.stf) | `.devil` |

Type 300 vs 1200 (both scripts): 300 = character scripts (`00 Frank`, `04 Marie`), 1200 = area/mob scripts
(`04_Cortijo`, `Template_MOB`) - inferred from names only. The 600/700 split is skeleton/mesh vs animation (names). The names
`scra`/`scrb`/`tre`/`mapdata`/`een`/`veg` are grptool conventions; their internal formats are not reversed yet.

## 3. `English.stf` - string table

```
u32 version = 3
u32 one     = 1
u32 count   = 0xcd1 (3281)
struct { u32 offset; u32 length; u32 fileSize(=254846, constant); u32 zero; } strings[count]   // from byte 12
<string data>   // starts at 12 + 16*count = 0xcd1c, concatenated, NOT NUL-terminated
```

Offsets are absolute and contiguous; the last string ends at EOF. The id is the record index (the `.devil` token file and
scripts refer to those ids, e.g. `DEVILDOG=1986`). Encoding Windows-1252. Strings contain inline markup such as
`{c16711680}` (colour as decimal RGB). Fields 3/4 (always file size / 0) are not understood and unused.
`grptool stf` outputs `id<TAB>text` (UTF-8, `\n` escaped).

## 4. Results on the demo (grptool list)

7044 resources, 577 MB extracted to `extracted/<grp>/<name>.<ext>`:

```
tga 1614   b3d 3915   anim.gr2 448   een 285   mesh.gr2 99   ini 125   scrb 83   veg 119   wav 113
mapdata 39 tre 39      bmp 39        emap 38   scra 36       gui 25    sndscript 25   g3d 1   devil 1
```

(ext breakdown sums to 7044; no dds, no ogg, no plain-text .txt resources besides ini/sndscript/g3d/devil.)

## 5. Open questions

* Internal formats: `B3D`, `TRE`, `EMAP`, mapdata, `EEN2`, `VEG`, `GUI`, compiled scripts (`scra`/`scrb`), sound scripts.
* Meaning of type ids 300 vs 1200, 1700 header, and of the `flag` (always 1) and stf fields 3/4.
* Loader behaviour for `Override\` directory and `cache\` (not analysed; both are empty in this install).
* Ghidra MCP backend was down during the session; the loader was read via REA's Ghidra provider only.
