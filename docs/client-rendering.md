# Client rendering (original assets)

The raylib client (`engine/client`) can draw the original Van Buren maps from the installed game data. It sits
behind `IRenderer`; without a game directory the old placeholder box renderer is used.

## Running

```
f3_client --viewmap 03_Tutorial_Junktown --gamedir F:\_work\F3_Demo      # no server
f3_client --host 127.0.0.1 --port 24555 --gamedir F:\_work\F3_Demo       # network mode
```

`F3_GAMEDIR` sets the default game directory. `--viewmap` takes a tre base name (`04_Tutorial_Vault`) or an emap
name (`00_04_Tutorial_Vault`, optional `.map`), case-insensitive. In network mode the renderer loads the map named
by `MapInfo.name` once; an unknown name (or no gamedir) falls back to the placeholder renderer.

Test options: `--shot FILE.png [--shot-frame N] [--shot-exit]`, `--cam AZ,EL,FOV`, `--target X,Z` (original
coordinates), `--clipy Y`, `--noentities`, `--modelscale S`, `--size W,H`.

Keys: RMB drag orbit, MMB / WASD / arrows pan, wheel = fov (zoom), Q/E rotate, R/F tilt, PgUp/PgDn/End roof
cut-away, G grid overlay, H props on/off, L lightmap gain, Space recentre, F12 screenshot. In network mode camera
keys are disabled while typing chat.

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
PgUp/PgDn cut roofs). World unit = metre; the walk grid has 2 cells per unit (cell centre
`((cx+.5)/2, -(cy+.5)/2)`). B3D models are scaled by `MapView::modelScale = 0.3` (a human comes out about 1.8 m).
Entity yaw: `MatrixRotateY(-yaw)`. Server tile to world: `((tx+.5)*cw, 0, -(ty+.5)*ch)` with `cw = gridW*0.5/mapWidth`.

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

* Static pose only: no Granny/skeletal animation (critters are in T-pose), no particles, VFX, decals, water.
* Model facing of critters is unverified (front/back of the T-pose could not be confirmed); network critters all
  use `HumMNor` tinted by colour because `CritterAdd` carries no model id.
* No per-light dynamic lighting (only baked lightmaps plus a sun term on props); sky is a quad/colour.
* Ground picking uses up-facing triangles only; multi-storey interiors pick the first hit from the camera.
* Original material state (ADD blends, fog, per-material alpha ref) is approximated by the flag mapping.
* 39 resources typed as TGA are actually BMP and are not decoded.
