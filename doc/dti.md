# MT Framework DTI (Dynamic Type Information) System

**Status: Native reference. Reviewed 2026-07-23.** This describes the analyzed
retail executable, not plugin-owned types. Packed DTI size is decoded by current
code as `(flagsAndSize & 0x7FFFFF) << 2`; names and addresses remain
reverse-engineering evidence rather than a stable API.

## Constructor recheck — 2026-10-26

Fresh read-only decompilation of `MtDTI__MtDTI @ 0x14045D860` on Ghidra
**8193**, `umvc3.exe.unpacked.exe`, confirms the size packing:

```c
sizeBits = (uint32_t)(byteSize >> 2) & 0x007FFFFF;
decodedByteSize = (flagsAndSize & 0x007FFFFF) << 2;
```

The inverse is exact only for four-byte-aligned sizes representable in the
23-bit field (maximum decoded size `0x01FFFFFC`). The constructor discards low
two bits and masks excess high bits; it does **not** round a byte size up.
Bits 23..28 receive `(param_7 & 0x3F)` and bits 29..31 receive the shifted
`param_6` flags. Do not interpret the complete `+0x30` dword as a byte count.

The constructor also qualifies the hash description below: if `param_5 == 0`,
it computes CRC with seed `0xFFFFFFFF` and masks `0x7FFFFFFF`; otherwise it
stores the supplied `param_5` directly at `+0x34`. Thus "always masked CRC"
describes the computed-name path, not every constructor invocation.

Current `rollback_snapshot.cpp` calls `DecodeDtiSize` in dynamic-list,
character, and camera sizing and applies caller-specific bounds/fallbacks. Its
helper definition was outside this pass's permitted reads, so this constructor
check establishes the native encoding rather than freshly auditing every plugin
decoder. DTI size alone does not validate an address, ownership, external buffers,
or the safety of copying an entire object. Other tables/debug-tool claims below
remain the earlier reference evidence; no full registration census, runtime
enumeration, build, or Ghidra datatype/signature mutation was performed.

## Overview

UMVC3 uses Capcom's MT Framework engine which includes a runtime type
information system called **DTI** (Dynamic Type Information). Every game
object class has a static `MtDTI` structure that describes its type name,
size, and place in the class hierarchy. These DTIs are registered at startup
in a global hash table and linked into a type tree.

The DTI system is **separate from the `MtProperty` system** — DTI describes
*types* (classes), while MtProperty describes *per-instance data fields*.

---

## DTI Structure (`MtDTI`)

From `MtDTI__MtDTI` constructor (`0x14045d860`):

| Offset | Size | Field       | Description                                |
|--------|------|-------------|--------------------------------------------|
| +0x00  | 8    | vtable      | Virtual function table                     |
| +0x08  | 8    | className   | `char*` — class name string                |
| +0x10  | 8    | nextSibling | Next sibling in parent's child list        |
| +0x18  | 8    | firstChild  | First child DTI (subclass)                 |
| +0x20  | 8    | parent      | Parent DTI (superclass)                    |
| +0x28  | 8    | hashNext    | Next entry in global hash table bucket     |
| +0x30  | 4    | flags/size  | Packed: type flags + instance size         |
| +0x34  | 4    | hash        | Supplied ID, or computed CRC32 name hash (`& 0x7FFFFFFF`) |

### DTI Initialization

`MtDTI__MtDTI(param_1, className, parent, ...)`:
1. Sets vtable to `PTR_FUN_140a69ed8`
2. Stores `className` at `+0x08`
3. Stores `parent` at `+0x20` (or `&PTR_PTR_140d22a28` if null — the root)
4. Inserts itself into the parent's child list (sibling chain via `+0x10`)
5. Stores supplied nonzero hash, or computes
   `MtCRC__getCRC(className, 0xFFFFFFFF) & 0x7FFFFFFF` → `+0x34`
6. Inserts itself into the global hash table at `bucket[hash & 0xFF]` (chain via `+0x28`)

### Type Hierarchy

```
PTR_PTR_140d22a28 (root sentinel)
 │
 ├── MtObject           (base class for everything)
 │    ├── sMain
 │    ├── sBattleSetting
 │    │    ├── sBattleSetting::cChr
 │    │    ├── sBattleSetting::cMainChr
 │    │    ├── sBattleSetting::cSubChr
 │    │    └── sBattleSetting::cOnePlay
 │    ├── sMvc3Manager
 │    ├── uBattleUiPause
 │    ├── uBattleUiTraining
 │    ├── cChrTeam
 │    ├── ...
 │    └── (thousands more)
```

---

## Global Hash Table

### Location

The global table is at `0x140D6D7B0`: 256 `qword` bucket heads (2048 bytes).

Referenced in:
- `MtDTI__findByHash` (`0x14045d980`): `(&MtDTI__globalHashTable)[hash & 0xFF]`
- `FUN_14045DB70`: walks all 256 buckets and follows each chain, but has no
  visible callback and returns no entries
- `MtDTI__MtDTI` constructor: inserts new DTI into the table

The exact address can be found by disassembling `MtDTI__findByHash`:
```
14045d980: LEA RDX,[0x140D6D7B0]
14045d987: MOVZX EAX,CL
14045d98a: MOV RAX,[RDX + RAX*8]
```

### Lookup (`MtDTI__findByHash`)

```
MtDTI__findByHash(uint hash)
    bucket = globalHashTable[hash & 0xFF]
    while (bucket != NULL):
        if (bucket->hash == hash):
            return bucket
        bucket = bucket->hashNext
    return NULL
```

Used 85+ times across the codebase for type lookups during initialization.

### Bucket Walker (`FUN_14045DB70`)

Walks all 256 buckets and their linked lists but performs no externally visible
per-entry action in the recovered instructions. It is not an enumeration API.
Hook code can enumerate types by reading `0x140D6D7B0` directly and following
`+0x28`, provided registration has completed.

---

## CRC32 Hash (`MtCRC__getCRC`)

**Address:** `0x1404c4590`

```c
ulonglong MtCRC__getCRC(byte* str, ulonglong initial)
{
    ulonglong i = 0;
    while (str[i] != 0) {
        initial = crc32Table[initial & 0xFF ^ str[i]] ^ ((initial >> 8) & 0xFFFFFF);
        i++;
    }
    return initial & 0xFFFFFFFF;
}
```

Called with `initial = 0xFFFFFFFF` for DTI hashing, then masked with
`0x7FFFFFFF` to keep it positive.

### Lookup Table

**Address:** `0x140b820f0` — 256 DWORD entries (1024 bytes).

The polynomial can be determined from the table values:
- Entry 0x00: `0x00000000`
- Entry 0x01: `0x77073096`
- Entry 0x02: `0xEE0E612C`
- Entry 0x03: `0x990951BA`

These values match **standard CRC32** (IEEE 802.3 / zip / gzip),
polynomial `0xEDB88320` (reflected).

### Hash Computation for DTI

```c
uint32_t MtDti_Hash(const char* name) {
    return MtCRC__getCRC((byte*)name, 0xFFFFFFFF) & 0x7FFFFFFF;
}
```

Clearing the top bit (masking with `0x7FFFFFFF`) ensures the hash is always
a non-negative signed integer.

---

## DTI Utility Functions Summary

| Address      | Name                          | Description                           |
|-------------|-------------------------------|---------------------------------------|
| `0x14045d860` | `MtDTI__MtDTI`               | Constructor — registers type          |
| `0x14045da80` | `MtDTI__init`                | Initialize hash table + sort children |
| `0x14045da90` | `FUN_14045da90`              | Sort child DTIs alphabetically        |
| `0x14045d980` | `MtDTI__findByHash`          | Lookup DTI by CRC32 hash              |
| `0x14045db70` | `FUN_14045DB70`               | Traverse all bucket chains; no callback |
| `0x1402a40c0` | `MtDTI__getID`                | Return the hash/ID at `DTI+0x34`      |
| `0x140455640` | *(clamp function)*            | Misidentified — not instance count    |
| `0x1404c4590` | `MtCRC__getCRC`              | CRC32 hash (standard polynomial)      |

---

## DTI vs. MtProperty

| System       | Purpose                       | Mechanism                              |
|-------------|-------------------------------|----------------------------------------|
| **DTI**     | Runtime type hierarchy        | Static structures, global hash table   |
| **MtProperty** | Per-instance named fields   | Linked list per object, getter/setter  |

DTI knows the *type*, MtProperty knows the *data*. They are complementary:

```c
// DTI: what type is this?
MtDTI* dti = object->getDTI();       // → "sBattleSetting"
while (dti->parent) {
    dti = dti->parent;               // walk up the hierarchy
}

// MtProperty: what fields does this instance have?
// (accessed via the property list, not DTI)
```

---

## Debug / Editor Remnants

### Collision DTI Tools (`nCollision`)

The collision system has debug DTI editor UI elements:

#### `MtCollisionUtil::MtDtiObject` (at `0x1409bc..`)

A wrapper around a DTI reference with editable properties:

| Property     | Type          | Getter             | Setter              |
|-------------|---------------|--------------------|---------------------|
| `DtiName`   | string (0x0e) | `FUN_1409be8b0`    | `FUN_1409befb0`     |
| `DtiID`     | int32 (0x06)  | `FUN_1409be8d0`    | `FUN_1409beff0`     |

#### `MtCollisionUtil::MtDtiSelecterIntermediate` (at `0x1409c..`)

A DTI type selector (dropdown/list) with edit capabilities:

| Property         | Type          | Getter             | Setter              |
|-----------------|---------------|--------------------|---------------------|
| `DtiDropDownList`| int32 (0x06)  | `FUN_1409be8d0`    | `FUN_1409c2990`     |
| `EditDtiName`   | string (0x0e) | `FUN_1409c2910`    | `FUN_1409c29b0`     |
| `EditDtiID`     | int32 (0x06)  | `FUN_1409be8d0`    | `FUN_1409c2990`     |

These are registered as named properties via `createProperty` on their
respective classes, making them visible to any MtProperty debug viewer
that walks the property list.

### `nCollision::cCollisionNode` EditDTI Property

The `EditDTI` property (type `0x83000e`, custom getter/setter) on
collision nodes allows changing the DTI of a collision node at runtime.
This is used by the collision editor tool.

---

## Using DTI from the Hook DLL

### Enumerating All Types

```c
// Global hash table address — find via MtDTI__findByHash disassembly
// Accesses globalHashTable[hash & 0xFF]
// Preferred VA in this executable. Relocate it at runtime.
extern uint64_t MtDTI__globalHashTable[256]; // 0x140D6D7B0

struct MtDTI {
    uint64_t vtable;
    const char* className;
    MtDTI* nextSibling;     // +0x10
    MtDTI* firstChild;      // +0x18
    MtDTI* parent;          // +0x20
    MtDTI* hashNext;        // +0x28
    uint32_t flagsAndSize;  // +0x30; low 23 bits store size / 4
    uint32_t hash;          // +0x34 — supplied ID, or CRC32 & 0x7FFFFFFF
};

// Walk all registered types:
for (int i = 0; i < 256; i++) {
    for (MtDTI* dti = MtDTI__globalHashTable[i]; dti; dti = dti->hashNext) {
        // dti->className tells you the type name
    }
}
```

Instance size is decoded as:

```c
size_t instanceSize = (dti->flagsAndSize & 0x007FFFFF) << 2;
```

The constructor at `0x14045D860` receives a byte size, shifts it right by two,
and stores the low 23 bits. This implies DTI-represented sizes are four-byte
granular.

### Walking the Type Hierarchy

```c
void WalkHierarchy(MtDTI* dti, int depth) {
    for (MtDTI* child = dti->firstChild; child; child = child->nextSibling) {
        printf("%*s%s\n", depth * 2, "", child->className);
        WalkHierarchy(child, depth + 1);
    }
}

// Start from root:
WalkHierarchy(rootDti, 0);
```

### Looking Up a Type by Name

```c
uint32_t HashName(const char* name) {
    // Call MtCRC__getCRC @ 0x1404c4590
    typedef uint64_t(__fastcall* MtCRC_t)(const char*, uint64_t);
    MtCRC_t getCRC = (MtCRC_t)0x1404c4590;
    return (uint32_t)(getCRC(name, 0xFFFFFFFF) & 0x7FFFFFFF);
}

// Then use MtDTI__findByHash @ 0x14045d980
typedef uint64_t(__fastcall* FindByHash_t)(uint32_t);
FindByHash_t findByHash = (FindByHash_t)0x14045d980;
uint64_t dti = findByHash(hash);
```

---

## Limitations (Retail Build)

The retail UMVC3 binary has **no general-purpose DTI debug viewer** built in.
While the complete DTI infrastructure exists (hash table, hierarchy, lookup),
there is no in-game UI that browses or edits DTI types. The engine-level
`MtDebug` system is minimal (assert/abort only, no interactive debugger).

The collision system has isolated DTI editing tools (`MtDtiObject`,
`MtDtiSelecterIntermediate`), but these are specific to the collision editor
and not a general type browser.

To build a DTI inspector, one would need to:
1. Walk the global hash table to enumerate all registered types
2. Build an ImGui tree view of the type hierarchy
3. Optionally hook object creation to track live instances per type
4. For selected instances, walk the MtProperty list to show/edit fields
