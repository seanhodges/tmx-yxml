# Plan: Replace libxml2 with yxml in tmx

## Overview

This plan replaces all libxml2 usage with yxml, a tiny (~1 KB) zero-allocation
pull parser. libxml2 is used in three places today:

| File | libxml2 API | Purpose |
|------|-------------|---------|
| `src/tmx_xml.c` | `<libxml/xmlreader.h>` — xmlTextReader streaming API | Parse .tmx/.tsx/.tx XML |
| `src/tmx_hash.c` | `<libxml/hash.h>` — xmlHashTable | Property and resource-manager hashtables |
| `src/tmx_mem.c` | `<libxml/xmlmemory.h>` — xmlMemSetup | Route libxml2 allocations through custom allocator |

In addition, the build system references libxml2:

| File | Reference |
|------|-----------|
| `CMakeLists.txt` | `find_package(LibXml2 REQUIRED)`, `target_link_libraries(tmx LibXml2::LibXml2)` |
| `tmxConfig.cmake.in` | `find_dependency(LibXml2)` |

The migration is split into four phases. Each phase ends with a buildable,
testable project. **No public API changes are made** — `tmx.h` is untouched.

---

## Phase 1 — Vendor yxml and Replace the Hashtable ✅ COMPLETE

**Goal:** Eliminate all non-parser libxml2 usage. Vendor yxml sources into the
tree so they are available for Phase 3.

### 1.1 Vendor yxml

- [x] Add `src/yxml.c` and `src/yxml.h` to the source tree.
- [x] Add `"src/yxml.c"` to the `add_library(tmx ...)` source list in
      `CMakeLists.txt`. libxml2 remains linked for now.

### 1.2 Replace the hashtable (`src/tmx_hash.c`)

The existing implementation wraps `xmlHashCreate`, `xmlHashUpdateEntry`,
`xmlHashLookup`, `xmlHashRemoveEntry`, `xmlHashFree`, and `xmlHashScan`.

- [x] Rewrite `tmx_hash.c` with a self-contained open-addressing or chaining
      hashtable (chaining with djb2 hash). Implements exactly the same internal
      API declared in `tmx_utils.h`.
- [x] Remove `#include <libxml/hash.h>` from `tmx_hash.c`.

### 1.3 Remove libxml2 memory integration (`src/tmx_mem.c`)

- [x] Remove `#include <libxml/xmlmemory.h>`.
- [x] Delete the `setup_libxml_mem()` function body.
- [x] Remove the `setup_libxml_mem()` declaration from `tmx_utils.h`.
- [x] Remove every call site of `setup_libxml_mem()`.

### 1.4 Verify

- [x] `cmake --build .` succeeds (libxml2 still linked, but only used by
      `tmx_xml.c`).
- [x] `dumper b64zlib.tmx` passes (alloc count 19 — matches baseline).
- [x] `dumper --use-rc-mgr ...` passes (alloc count 113 — matches baseline).
- [x] Alloc counts match pre-migration baseline exactly.

---

## Phase 2 — Implement the yxml Pull Parser ✅ COMPLETE

**Goal:** Rewrite `src/tmx_xml.c` to use yxml instead of xmlTextReader.
This is the largest phase. The new parser must produce byte-identical TMX
data structures for every input the old parser accepted.

### 2.1 Input abstraction ✅

Simplified design: all input variants slurp the entire document into a buffer.
yxml then processes the buffer byte-by-byte.

```c
typedef struct {
    char       *buf;
    size_t      buf_len;
    int         owned;
} xml_reader;
```

- [x] `reader_open_file(const char *path)` — opens file, reads into buffer.
- [x] `reader_open_fd(int fd)` — reads fd into buffer.
- [x] `reader_open_callback(tmx_read_functor cb, void *ud)` — drains callback
      into buffer.
- [x] `reader_close(xml_reader *r)` — frees any owned buffer.

Note: `reader_open_buffer` was not needed — buffer entry points pass the
buffer directly to the document parser without wrapping.

### 2.2 yxml state-machine core ✅

Implemented with dynamically-grown buffers instead of fixed-size arrays:

```c
typedef struct {
    xml_attr attrs[MAX_ATTRS];
    int      attr_count;
    char    *attr_name;   /* dynamically grown */
    char    *attr_value;  /* dynamically grown */
    char    *content;     /* dynamically grown */
    /* Event state machine */
    char     elem_name[256];
    int      in_attrs;
    int      pending_end;
    int      pending_start;
    char     pending_elem[256];
    int      pending_content;
} xml_accum;
```

Key function: `next_event()` translates yxml's byte-level events into
higher-level events (EVT_ELEM_START, EVT_ELEM_END, EVT_CONTENT, EVT_EOF,
EVT_ERROR). Uses pending flags to handle multi-event transitions (e.g.,
self-closing elements need EVT_ELEM_START then EVT_ELEM_END).

- [x] YXML_ELEMSTART → push depth, record element, accumulate attrs
- [x] YXML_ATTRSTART/ATTRVAL/ATTREND → accumulate attribute name/value pairs
- [x] YXML_CONTENT → accumulate content, handle first-char transition from attrs
- [x] YXML_ELEMEND → handle self-closing elements, content delivery, depth pop

### 2.3 Element handlers ✅

All element handlers rewritten:

- [x] `parse_map_document_y` / `parse_map_y`
- [x] `parse_tileset_list_y` (external .tsx via fresh yxml instance)
- [x] `parse_tileset_y` / `parse_tileset_document_y`
- [x] `parse_tile_y`
- [x] `parse_animation_y`
- [x] `parse_layer_y` (all layer types)
- [x] `parse_data_y`
- [x] `parse_object_y` (with template .tx loading)
- [x] `parse_image_y` (via `parse_tileoffset_y`)
- [x] `parse_points_y`
- [x] `parse_text_y`
- [x] `parse_properties_y` / `parse_property_y`
- [x] `parse_template_document_y` / `parse_template_y`

### 2.4 Public entry points ✅

All 16 public entry points preserved with identical signatures:

- [x] `parse_xml`, `parse_xml_buffer`, `parse_xml_buffer_vpath`,
      `parse_xml_fd`, `parse_xml_fd_vpath`, `parse_xml_callback`,
      `parse_xml_callback_vpath`
- [x] `parse_tsx_xml`, `parse_tsx_xml_buffer`, `parse_tsx_xml_fd`,
      `parse_tsx_xml_callback`
- [x] `parse_tx_xml`, `parse_tx_xml_buffer`, `parse_tx_xml_fd`,
      `parse_tx_xml_callback`

### 2.5 Remove all libxml2 references from parser ✅

- [x] Removed `#include <libxml/xmlreader.h>` from `tmx_xml.c`.
- [x] Added `#include "yxml.h"` to `tmx_xml.c`.
- [x] No other source file includes any `<libxml/...>` header.

### 2.6 Verify ✅

- [x] `cmake --build .` succeeds.
- [x] `dumper b64zlib.tmx` — correct output (property ordering differs due to
      new hashtable, which is expected).
- [x] `dumper --use-rc-mgr --fd tileset.tsx --callback pointtemplate.tx
      --buffer tiletemplate.tx objecttemplates.tmx` — correct output.
- [x] `dumper csv.tmx` and `dumper externtileset.tmx` — correct output.
- [x] Mem alloc counts improved vs baseline (2 vs 19, 0 vs 113) — no libxml2
      global state overhead.

---

## Phase 3 — Build System Cleanup ⬜ PENDING

**Goal:** Remove all libxml2 traces from the build configuration so the project
builds without libxml2 installed.

### 3.1 CMakeLists.txt

- [x] Remove `find_package(LibXml2 REQUIRED)`.
- [x] Remove `target_link_libraries(tmx LibXml2::LibXml2)`.
- [x] Verify `"src/yxml.c"` is already in the `add_library` source list
      (added in Phase 1).

### 3.2 tmxConfig.cmake.in

- [x] Remove `find_dependency(LibXml2)`.

### 3.3 CI workflow (`.github/workflows/buildandtest.yaml`)

- [x] Verify the workflow does not install libxml2 explicitly (it doesn't
      today — it relies on the system package). No changes expected, but
      confirm the build passes in a clean environment without libxml2-dev.

### 3.4 Verify

- [x] Full clean build from scratch (`rm -rf build && cmake -B build && cmake
      --build build`) succeeds without libxml2 headers/libs present.
- [ ] All dumper tests pass.
- [ ] C++ compatibility test passes.

---

## Phase 4 — Final Verification and Hardening ⬜ PENDING

**Goal:** Confirm 100% behavioral compatibility, no memory leaks, and no
regressions.

### 4.1 Functional tests

- [ ] `dumper b64zlib.tmx` — basic map with base64+zlib data.
- [ ] `dumper --use-rc-mgr --fd tileset.tsx --callback pointtemplate.tx
      --buffer tiletemplate.tx objecttemplates.tmx` — resource manager with
      fd, callback, and buffer loading.
- [ ] Test with all example data files: `csv.tmx`, `b64zstd.tmx`,
      `externtileset.tmx`.

### 4.2 Memory leak check

- [ ] Verify the dumper reports `0 mem alloc not freed` for every test run.
- [ ] Run under valgrind (if available): `valgrind --leak-check=full
      ./dumper b64zlib.tmx` — expect zero leaks and zero errors.

### 4.3 API compatibility

- [ ] `tmx.h` is byte-identical to upstream (no modifications).
- [ ] C++ compatibility test: `c++ -I src/ -c test.cpp` compiles cleanly.
- [ ] All function signatures in `tmx_utils.h` (the internal API) are
      preserved exactly, except for the removal of `setup_libxml_mem()`.

### 4.4 Edge cases

- [ ] Verify error paths: corrupt XML produces clear error messages via
      `tmx_errno` / `tmx_strerr()`.
- [ ] Verify empty layers, empty tilesets, empty object groups are handled.
- [ ] Verify self-closing XML elements (`<ellipse/>`, `<point/>`) parse
      correctly.

---

## Files Modified (Summary)

| File | Phase | Change |
|------|-------|--------|
| `src/yxml.c` | 1 | **New** — vendored |
| `src/yxml.h` | 1 | **New** — vendored |
| `src/tmx_hash.c` | 1 | Rewritten — custom hashtable, no libxml2 |
| `src/tmx_mem.c` | 1 | Remove `setup_libxml_mem()` and libxml2 include |
| `src/tmx_utils.h` | 1 | Remove `setup_libxml_mem()` declaration |
| `src/tmx_xml.c` | 2 | Rewritten — yxml-based parser |
| `CMakeLists.txt` | 1, 3 | Add yxml.c (Phase 1), remove libxml2 (Phase 3) |
| `tmxConfig.cmake.in` | 3 | Remove `find_dependency(LibXml2)` |

**Files NOT modified:**

| File | Reason |
|------|--------|
| `src/tmx.h` | Public API — must not change |
| `src/tmx.c` | No libxml2 usage — unchanged |
| `src/tmx_utils.c` | No libxml2 usage — unchanged |
| `src/tmx_err.c` | No libxml2 usage — unchanged |

---

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| yxml attribute values or text content exceed fixed buffer sizes | Use dynamically-grown buffers via `tmx_alloc_func` for content that can be arbitrarily large (layer data, polygon points). Use generous fixed buffers only for element/attribute names which are bounded by the TMX spec. |
| yxml does not handle XML entities (`&amp;`, `&lt;`, etc.) in text content | yxml handles the five standard XML entities. Custom DTD entities are not used by TMX files. |
| Recursive template/tileset loading could cause stack overflow | The original code has the same recursion pattern. The replacement mirrors it identically. |
| Subtle behavioral differences in attribute parsing order | The new parser accumulates all attributes of an element before processing, matching the random-access semantics of the original `xmlTextReaderGetAttribute()`. |
| The hashtable replacement has different collision behavior | Use a well-tested hash function (djb2 or FNV-1a). Property counts per element are small (typically < 20), so performance is not a concern. |
