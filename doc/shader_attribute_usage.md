# Model shader attributes: usage research

Research: 2026-10-15, Ghidra `umvc3.exe.unpacked.exe` and installed native
resources. This is follow-up to `character_headless_render_audit_20261015.md`
and the `shader_upload_ab.md` experiment. No runtime behavior changed.

## Result

Ordinary models use material shaders. The A/B experiment targets a DIFFERENT,
optional feature: a texture of four packed attribute channels per joint, enabled
by model property `mShaderAttributes`. No concrete model, character or move
enabling that optional texture has yet been established.

The feature is present in both native rendering code and the installed shader
package, so it is not merely an unimplemented getter/setter. That does not prove
normal gameplay selects it. The current runtime capture observed zero enabled
upload calls among 16,251 simulation-thread calls across both peers.

## Native producer and enablement

| Location | Confirmed behavior |
|---|---|
| `uBaseModel__ctor @ 0x14055B0F0`, instruction `0x14055B648` | Clears bit 29 at model `+0x124`: feature defaults off. |
| `uBaseModel__setShaderAttributes @ 0x140562CF0` | Writes `(old & 0xDFFFFFFF) | ((value & 1) << 29)`. |
| `uBaseModel__isShaderAttributes @ 0x14055FA80` | Returns that bit. |
| `uBaseModel__createProperty @ 0x14055B970` | Registers callback-backed Boolean-like property `mShaderAttributes`, using the getter/setter above. |
| `uModel__updateShaderAttributes @ 0x14056A860` | When enabled, creates the selected-context texture if missing, maps, populates and unmaps it. When disabled, releases an existing selected-context texture or returns cheaply. |

The setter's recovered references are property-registration data references,
not direct native character/move calls. Searches scoped to Model-named functions
for `0x20000000`, `0xDFFFFFFF` and shift `0x1D` found no additional direct
enable operation at model `+0x124`. Matches at motion `+0x558` or particle
`+0x70` are unrelated flags and must not be treated as users of this feature.

Neither inspected binding body, `uBaseModel__setModel @ 0x140561CE0` nor
`uModel__setModel @ 0x140568670`, directly assigns/copies the render flag or
calls its setter. Binding a MOD resource is therefore not, by itself, a proven
enablement path. Transitive `initRCN`, bulk copies, unnamed functions, byte-level
writes and dynamic property dispatch are not exhaustively excluded.

## Native consumer and shader names

`uModel__setCommonState @ 0x140566610` checks bit 29 and a non-null selected
texture at model `+0x1250/+0x1258`, then selects the optional shader variant and
binds the texture. Its early RCN/texture-derived geometry branch can bypass this
block; it is not an unconditional draw binding.

Read-only parsing of installed `nativePCx64/UserShader/UserShaderPackage.mfx`
(1,898,057 bytes) resolves the exact engine IDs:

| Engine ID | Package name | Role |
|---|---|---|
| `0x38A` | `tShaderAttributes` | Optional per-joint attribute texture. |
| `0x62C` | `FShaderAttributes` | Base shader-feature entry. |
| `0x62D` | `FShaderAttributesVTF` | Variant selected by native optional texture binding. VTF denotes vertex texture fetch. |
| `0x33F` | `tVertexPositionMap` | Separate texture-derived geometry path. |
| `0x341` | `tVertexNormalMap` | Separate geometry normal map. |
| `0x342` | `tVertexTangentMap` | Separate geometry tangent map. |

These are engine package identifiers, NOT hardware texture-register numbers.
The native draw state references the `0x62D` entry when the optional attribute
texture is available, versus the base `0x62C` entry otherwise; the texture
binding uses semantic `0x38A`.

Package decoding was checked against each record's embedded ID: the u64 offset
table begins at file `+0x20`, uses index `ID - 1`, and points to records whose
first u64 is the name offset relative to the string table. String-table base
is the u32 at header `+0x10`; the embedded u16 ID is at record `+0x16`.
Record names and native binding IDs agree independently.

## Attribute data is not ordinary skinning

Existing `uModel__Joint` is `0xC0` bytes, with packed uint attributes at `+0xB0`.
Each joint produces one texel of the 256-by-1 texture:

```text
float4((v & 255), ((v >> 8) & 255), ((v >> 16) & 255), (v >> 24)) / 255
```

This proves four unsigned normalized channels per joint. It does NOT establish
that their semantic meanings are diffuse color, alpha, emission, dissolve or
any particular character effect. Earlier shorthand "per-joint color data"
should not be read as a verified channel interpretation.

The package also contains normal skinning inputs and joint-matrix constants.
Those, ordinary MaterialChar shaders and the position/normal/tangent maps above
are not evidence that optional `mShaderAttributes` is enabled.

Two `+0xB0` stores in `uModel__setupMotion`, at `0x140529B7B` and
`0x14052A540`, remain writer leads only: destination bases/source semantics
were not verified before the Ghidra connection became unresponsive.
`uModel__Joint__createProperty @ 0x14056DA70` also needs inspection around
`0x14056DEA3`. Neither lead proves model-level enablement.

## Installed resource evidence

Inspected 175 archives: character common, base-costume and parameter archives,
19 stage archives and two boot archives. Selected payloads were decompressed
in memory without extraction or modification:

| Format | Payloads |
|---|---:|
| EFL | 2,001 |
| MRL | 1,022 |
| MOD | 1,022 |
| SDL | 309 |
| CAC | 406 |
| CST | 34 |

Total selected uncompressed data: 247,938,398 bytes. None contained literal
ASCII or UTF-16LE references to `mShaderAttributes`, `tShaderAttributes`,
`FShaderAttributes` or `FShaderAttributesVTF`. This is a literal-string scan,
NOT full deserialization or a negative proof for numeric/hashed/script-command
references. Alternate costumes and formats outside this set were not scanned.

Positive ordinary-material examples from base-costume MRLs:

| Character model | MaterialChar | MaterialCharAlpha |
|---|---:|---:|
| Ryu | 13 | 1 |
| Nemesis | 17 | 0 |
| Iron Man | 11 | 0 |
| Doctor Doom | 12 | 0 |
| Phoenix | 18 | 3 |
| Shuma-Gorath | 6 | 5 |
| Dormammu | 22 | 0 |
| Ghost Rider | 17 | 0 |

MRL header `+8` is material count, `+0xC` is texture count, and `+0x20`
is the u64 material-table offset. Inspected records have `0x48` stride.
Their class IDs match `(~CRC32("nDraw::" + className)) & 0x7FFFFFFF`.
These material classes establish ordinary rendering usage, not optional
attribute-texture enablement. For example, Ryu's material resources reference
toon, normal and base-map textures without a literal optional-feature property.

## Limits and next evidence

Ghidra began persistently timing out during property-loader and animation-writer
tracing. No attempt was made to restart or modify the user's Ghidra instance.
Consequently there is no complete SDL/LRP/property-loader-to-setter chain,
animation opcode mapping or global direct-write proof.

The next useful evidence is an actual bit-29 enable event with model DTI/resource
identity and caller, or completion of the writer/property-loader trace once
Ghidra responds. Setter-only instrumentation would miss direct/bulk flag writes;
seeing a non-default joint attribute alone would not prove the texture is used.
Do not force-enable the flag and treat the resulting cost as normal gameplay.

Current conclusion: ordinary material shading is widespread; the optional
shader-attribute texture is implemented but has no confirmed normal-gameplay
user in the inspected evidence. No new performance estimate is justified.
