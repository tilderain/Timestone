# Sound System Notes (UMVC3)

## Resource Types

From vfn6 (type identifier string) of each resource class:

| Type String | Class                     | Description           |
|-------------|---------------------------|-----------------------|
| `sbkr`      | rSoundBank                | Sound bank container  |
| `xsew`      | rSoundSourceMSADPCM       | MSADPCM-encoded sound |
| `sngw`      | rSoundSourceOggVorbis     | Ogg Vorbis sound      |
| (unknown)   | rSoundSourcePC            | Raw PCM?              |

The sound bank (`.sbkr`) is a container/index file. Individual sound entries reference
either MSADPCM (`.xsew`) or Ogg Vorbis (`.sngw`) resources loaded via cResource system.

## Loading Pipeline

Stage sounds are loaded via path pattern:
`sound\\se\\stg\\st%s\\st%s` (stage name, variant)
`sMvc3Sound__buildStagePath` at `0x1402b0a10`

`sMvc3Sound__allocAndDispatchLoad` at `0x1402a0a40` handles two loading types:
- **Type 1**: allocates via `FUN_1402a6a20`
- **Type 2**: allocates via `FUN_1402a4e40`

---

## .sbkr Format (rSoundBank)

### vtable:

| Offset | Name             | Description                 |
|--------|------------------|-----------------------------|
| vfn6   | rSoundBank_vfn6  | Returns `"sbkr"`            |
| vfn9   | rSoundBank_vfn9  | Iterates entries, releases  |
| vfn10  | rSoundBank_vfn10 | **READ** .sbkr from stream  |
| vfn11  | rSoundBank_vfn11 | **WRITE** .sbkr to stream   |
|        | rSoundBank__getDTI | Returns DTI descriptor    |
|        | rSoundBank__dtor | Destructor                  |

### rSoundBank Memory Layout (object offsets from `this`):

| Offset | Size | Type     | Description                             |
|--------|------|----------|-----------------------------------------|
| 0x70   | 4    | int32    | `count1` — number of sound entries      |
| 0x78   | 8    | ptr      | Table 1 pointer (entries of 0x18 bytes) |
| 0x80   | 4    | int32    | `count2` — number of resource entries   |
| 0x88   | 8    | ptr      | Table 2 pointer (entries of 0x58 bytes) |
| 0x90   | 4    | int32    | `count3` — number of aux entries        |
| 0x98   | 8    | ptr      | Table 3 pointer (entries of 8 bytes)    |

(offsets 0x00-0x6C are inherited from cResource base class)

### .sbkr File Format (binary):

```
Offset  Size  Field
------  ----  -----
0x00    4     Magic: 0x524B4253 = "SBKR" (little-endian bytes: 53 42 4B 52)
0x04    4     Version: 4 (int32)
0x08    4     count1: number of entries in Table 1 (int32)
0x0C    4     count2: number of entries in Table 2 (int32)
0x10    4     count3: number of entries in Table 3 (int32)
0x14         [Table 1 follows]
```

#### Table 1 — Sound Entries (`count1` × 0x18 = 24 bytes each)

```
Offset  Size  Description
------  ----  -----------
0x00    8     ID / some identifier (qword)
0x08    8     Relative pointer to Table 2 resource entry.
              Stored as index (offset): decoded as `value * 0x58 + table2_base`.
              Converted to absolute pointer on read.
0x10    8     Another qword value
```

#### Table 2 — Resource Entries (`count2` × 0x58 = 88 bytes each)

```
Offset  Size  Description
------  ----  -----------
0x00    8     Pointer to loaded cResource (populated at runtime, null before load)
0x08    4     Hash value (uint32) — used for MtDTI lookup
0x0C    0x4C  Data (76 bytes) — varies by resource type
```

On read:
1. A **name string** (up to 64 chars, null-terminated) is read separately from stream
2. Then 0x50 bytes are read into offset 0x08 of the entry
3. If name is non-empty: the hash at offset 0x08 is used for MtDTI::findByHash, then
   `cResource::load(path, dti, name, 1)` is called to resolve the actual resource pointer
4. If name is empty: the resource pointer at offset 0x00 stays null

#### Table 3 — Aux Entries (`count3` × 8 bytes each)

Simple array of qwords (8 bytes each). Purpose unknown — possibly timing/trigger data.

### vfn9 (iterate + release):

```
for each entry in Table 2:
    if resource pointer != null:
        if (flags & 1) == 0:
            cResource::release()
            return
        else:
            call vfn0x80 on the resource
            if result == false:
                call cleanup function
                return 0
return 1
```

### vfn10 (read/load):

1. Open stream with buffer size 0x1000
2. Read 0x14 bytes header
3. Validate magic (`0x524B4253`) and version (`4`)
4. Call cleanup function to reset internal state
5. Set count1, count2, count3 from header
6. Allocate Table 1: `count1 * 0x18` bytes
7. Allocate Table 2: `count2 * 0x58` bytes
8. Allocate Table 3: `count3 * 8` bytes
9. Read Table 1 entries (convert relative pointers to absolute)
10. For each Table 2 entry: read name string (0x40 max), read payload (0x50 bytes), resolve resource
11. Read Table 3 data

### vfn11 (write/save):

1. Create stream with buffer size 0x1000
2. Write header: magic, version, count1, count2, count3
3. Write Table 1 entries (convert absolute pointers back to relative indices)
4. For each Table 2 entry:
   - If resource pointer is null: write 0 for hash, write null terminator
   - If resource is loaded: write name string, write hash (from DTI), write entry payload
5. Write Table 3 data

---

## Sound Data Formats

### xsew (MSADPCM)

Class: `rSoundSourceMSADPCM`
Read function: `rSoundSourceMSADPCM_vfn36` at `0x1407fb0d0`

vfn10 (load) reads from stream, handles multiple container formats:
- Raw MSADPCM
- MSADPCM with header (`QWORD_140e17108`)
- Container format (`QWORD_140e170d0`)
- Container format with data size lookup

Sound source properties include data size at offset 0x100 and offset 0x90.

### sngw (Ogg Vorbis)

Class: `rSoundSourceOggVorbis`
Read function: `rSoundSourceOggVorbis_vfn36` at `0x1407fb740`

---

## DTI / Allocator Info

| Class                  | DTI Allocator Address | Notes                |
|------------------------|-----------------------|----------------------|
| rSoundBank             | `0x140e1dea0`         | Sound bank allocator |
| rSoundSourceMSADPCM    | `0x140e1df50`         | MSADPCM allocator    |
| rSoundSource (base)    | `rSoundSource__AllocDTI` | Base class allocator |

## Sound Loading by Type

From `sMvc3Sound__allocAndDispatchLoad` at `0x1402a0a40`:
- **Type 1**: MSADPCM sounds (constructed via `FUN_1402a6a20`)
- **Type 2**: Ogg Vorbis sounds (constructed via `FUN_1402a4e40`)
