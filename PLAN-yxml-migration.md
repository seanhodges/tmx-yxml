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

## Phase 1 — Vendor yxml and Replace the Hashtable

**Goal:** Eliminate all non-parser libxml2 usage. Vendor yxml sources into the
tree so they are available for Phase 3.

### 1.1 Vendor yxml

- [ ] Add `src/yxml.c` and `src/yxml.h` to the source tree.
- [ ] Add `"src/yxml.c"` to the `add_library(tmx ...)` source list in
      `CMakeLists.txt`. libxml2 remains linked for now.

### 1.2 Replace the hashtable (`src/tmx_hash.c`)

The existing implementation wraps `xmlHashCreate`, `xmlHashUpdateEntry`,
`xmlHashLookup`, `xmlHashRemoveEntry`, `xmlHashFree`, and `xmlHashScan`.

- [ ] Rewrite `tmx_hash.c` with a self-contained open-addressing or chaining
      hashtable. It must implement exactly the same internal API declared in
      `tmx_utils.h`:

  ```c
  void* mk_hashtable(unsigned int initial_size);
  void  hashtable_set(void *ht, const char *key, void *val,
                      hashtable_entry_deallocator dealloc);
  void* hashtable_get(void *ht, const char *key);
  void  hashtable_rm(void *ht, const char *key,
                     hashtable_entry_deallocator dealloc);
  void  hashtable_foreach(void *ht, hashtable_foreach_functor fn,
                          void *userdata);
  void  free_hashtable(void *ht, hashtable_entry_deallocator dealloc);
  ```

- [ ] Remove `#include <libxml/hash.h>` from `tmx_hash.c`.

### 1.3 Remove libxml2 memory integration (`src/tmx_mem.c`)

- [ ] Remove `#include <libxml/xmlmemory.h>`.
- [ ] Delete the `setup_libxml_mem()` function body (make it a no-op or remove
      it entirely).
- [ ] Remove the `setup_libxml_mem()` declaration from `tmx_utils.h`.
- [ ] Remove every call site of `setup_libxml_mem()` — these are in
      `tmx_xml.c` (one per public `parse_*` entry point) and in `tmx_hash.c`
      (`mk_hashtable`). Calls in `tmx_xml.c` will be fully cleaned up in
      Phase 3; for now just delete them.

### 1.4 Verify

- [ ] `cmake --build .` succeeds (libxml2 still linked, but only used by
      `tmx_xml.c`).
- [ ] `dumper b64zlib.tmx` passes.
- [ ] `dumper --use-rc-mgr --fd tileset.tsx --callback pointtemplate.tx
      --buffer tiletemplate.tx objecttemplates.tmx` passes.
- [ ] Memory alloc/free count is 0.

---

## Phase 2 — Implement the yxml Pull Parser

**Goal:** Rewrite `src/tmx_xml.c` to use yxml instead of xmlTextReader.
This is the largest phase. The new parser must produce byte-identical TMX
data structures for every input the old parser accepted.

### 2.1 Input abstraction

All existing load variants (file, buffer, fd, callback) must funnel into yxml,
which consumes raw bytes. Define a small internal "reader" abstraction:

```c
typedef struct {
    const char *buf;   /* non-NULL when parsing from a memory buffer */
    size_t      buf_len;
    size_t      buf_pos;
    int         fd;    /* >= 0 when parsing from fd */
    tmx_read_functor callback;
    void       *userdata;
    char        chunk[4096]; /* read-ahead buffer for fd / callback */
    int         chunk_len;
    int         chunk_pos;
} xml_reader;
```

- [ ] `reader_open_file(const char *path)` — opens file, reads into malloc'd
      buffer (TMX files are not typically large enough to warrant streaming).
- [ ] `reader_open_buffer(const char *buf, int len)` — wraps an existing buffer.
- [ ] `reader_open_fd(int fd)` — reads fd into a malloc'd buffer.
- [ ] `reader_open_callback(tmx_read_functor cb, void *ud)` — drains the
      callback into a malloc'd buffer.
- [ ] `reader_close(xml_reader *r)` — frees any owned buffer.

Reading the entire document into memory before parsing is acceptable because:
- The original xmlTextReader already buffers internally.
- TMX maps are typically < 10 MB.
- yxml processes one byte at a time with zero allocations, so the only memory
  cost is the document buffer itself.

### 2.2 yxml state-machine core

Implement the main event loop that drives yxml and dispatches to element
handlers. Key design:

```c
/* Accumulated attribute storage */
#define MAX_ATTRS 32
typedef struct {
    char name[128];
    char value[4096];
} xml_attr;

typedef struct {
    yxml_t       x;
    char         stack[8192];  /* yxml stack buffer */
    xml_attr     attrs[MAX_ATTRS];
    int          attr_count;
    char         content[65536]; /* text content accumulator */
    int          content_len;
    int          depth;         /* manual depth counter */
    const char  *current_elem;
    /* Parsing context */
    tmx_resource_manager *rc_mgr;
    const char  *filename;      /* for resolving relative paths */
} parse_context;
```

- [ ] On `YXML_ELEMSTART`: push depth, record element name, reset attr list and
      content buffer.
- [ ] On `YXML_ATTRSTART`: begin accumulating a new attribute name.
- [ ] On `YXML_ATTRVAL`: append character to current attribute value buffer.
- [ ] On `YXML_ATTREND`: finalize the current attribute name/value pair.
- [ ] On `YXML_CONTENT`: append character to the content accumulator.
- [ ] On `YXML_ELEMEND`: dispatch to the appropriate handler based on element
      name and depth, then pop depth.

The dispatch logic mirrors the existing `parse_map`, `parse_tileset`, etc.
functions but operates on accumulated attribute arrays instead of random-access
`xmlTextReaderGetAttribute()` calls. Implement a helper:

```c
static const char* get_attr(parse_context *ctx, const char *name);
```

That scans the accumulated attrs array — this is the direct replacement for
`xmlTextReaderGetAttribute()`.

### 2.3 Element handlers

Rewrite each of the existing parse functions. The logic and TMX struct
population remain identical — only the XML access pattern changes:

- [ ] `parse_map_document` — Verify root element is `<map>`.
- [ ] `parse_map` — Read map attributes, dispatch children (tileset, layer,
      properties).
- [ ] `parse_tileset_list` — Handle firstgid + external/embedded tilesets.
      For external .tsx: read the file into a buffer and recursively parse it
      with a fresh `yxml_t` + `parse_context`.
- [ ] `parse_tileset` — Read tileset attributes and children (image,
      tileoffset, tile, properties).
- [ ] `parse_tile` — Read tile attributes and children (properties, image,
      objectgroup/collision, animation).
- [ ] `parse_animation` — Collect `<frame>` elements. Use the same recursive
      stack-allocation trick as the original, or switch to a simpler two-pass
      (count then allocate) approach.
- [ ] `parse_layer` — Handle all layer types (L_LAYER, L_OBJGR, L_IMAGE,
      L_GROUP) and their children.
- [ ] `parse_data` — Read encoding/compression attributes, accumulate inner
      text content, pass to `data_decode()`.
- [ ] `parse_object` — Read object attributes, handle template references
      (opening .tx files inline), child elements (polygon, polyline, ellipse,
      text, point, properties).
- [ ] `parse_image` — Read source/height/width/trans attributes.
- [ ] `parse_points` — Read `points` attribute, delegate to existing point
      parser.
- [ ] `parse_text` — Read text attributes and inner text content.
- [ ] `parse_properties` / `parse_property` — Read property attributes,
      handle typed values, handle nested `<properties>` for class-typed
      properties.
- [ ] `parse_template_document` / `parse_template` — Handle .tx template files
      with tileset and object children.
- [ ] `parse_tileset_document` — Handle standalone .tsx files.

### 2.4 Public entry points

Rewrite the public `parse_xml*` / `parse_tsx_xml*` / `parse_tx_xml*` functions
to use the new reader + yxml infrastructure. The function signatures in
`tmx_utils.h` remain unchanged:

- [ ] `parse_xml(rc_mgr, filename)` — open file, parse map document.
- [ ] `parse_xml_buffer(rc_mgr, buffer, len)` — parse map from buffer.
- [ ] `parse_xml_buffer_vpath(rc_mgr, buffer, len, vpath)` — same with virtual path.
- [ ] `parse_xml_fd(rc_mgr, fd)` / `parse_xml_fd_vpath(...)` — read fd, parse.
- [ ] `parse_xml_callback(rc_mgr, cb, ud)` / `parse_xml_callback_vpath(...)` —
      drain callback, parse.
- [ ] `parse_tsx_xml(filename)` — open file, parse tileset document.
- [ ] `parse_tsx_xml_buffer(buffer, len)` — parse tileset from buffer.
- [ ] `parse_tsx_xml_fd(fd)` — read fd, parse tileset.
- [ ] `parse_tsx_xml_callback(cb, ud)` — drain callback, parse tileset.
- [ ] `parse_tx_xml(rc_mgr, filename)` — open file, parse template document.
- [ ] `parse_tx_xml_buffer(rc_mgr, buffer, len)` — parse template from buffer.
- [ ] `parse_tx_xml_fd(rc_mgr, fd)` — read fd, parse template.
- [ ] `parse_tx_xml_callback(rc_mgr, cb, ud)` — drain callback, parse template.

### 2.5 Remove all libxml2 references from parser

- [ ] Remove `#include <libxml/xmlreader.h>` from `tmx_xml.c`.
- [ ] Add `#include "yxml.h"` to `tmx_xml.c`.
- [ ] Confirm no other source file includes any `<libxml/...>` header.

### 2.6 Verify

- [ ] `cmake --build .` succeeds.
- [ ] `dumper b64zlib.tmx` output matches the pre-migration baseline.
- [ ] `dumper --use-rc-mgr --fd tileset.tsx --callback pointtemplate.tx
      --buffer tiletemplate.tx objecttemplates.tmx` output matches baseline.
- [ ] Memory alloc/free count is 0.

---

## Phase 3 — Build System Cleanup

**Goal:** Remove all libxml2 traces from the build configuration so the project
builds without libxml2 installed.

### 3.1 CMakeLists.txt

- [ ] Remove `find_package(LibXml2 REQUIRED)`.
- [ ] Remove `target_link_libraries(tmx LibXml2::LibXml2)`.
- [ ] Verify `"src/yxml.c"` is already in the `add_library` source list
      (added in Phase 1).

### 3.2 tmxConfig.cmake.in

- [ ] Remove `find_dependency(LibXml2)`.

### 3.3 CI workflow (`.github/workflows/buildandtest.yaml`)

- [ ] Verify the workflow does not install libxml2 explicitly (it doesn't
      today — it relies on the system package). No changes expected, but
      confirm the build passes in a clean environment without libxml2-dev.

### 3.4 Verify

- [ ] Full clean build from scratch (`rm -rf build && cmake -B build && cmake
      --build build`) succeeds without libxml2 headers/libs present.
- [ ] All dumper tests pass.
- [ ] C++ compatibility test passes.

---

## Phase 4 — Final Verification and Hardening

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
