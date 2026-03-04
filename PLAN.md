# Plan: Replace libxml2 with yxml in tmx

## Overview

We have extended this project to use yxml as a compile-time alternative to libxml.

Command to build it:
```
cmake -B build -D WANT_LIBXML=Off
```

## Final Verification and Hardening

**Goal:** Confirm 100% behavioral compatibility, no memory leaks, and no
regressions.

### 4.1 Functional tests

- [x] `dumper b64zlib.tmx` — basic map with base64+zlib data.
- [x] `dumper --use-rc-mgr --fd tileset.tsx --callback pointtemplate.tx
      --buffer tiletemplate.tx objecttemplates.tmx` — resource manager with
      fd, callback, and buffer loading.
- [x] Test with all example data files: `csv.tmx`, `b64zstd.tmx`,
      `externtileset.tmx`.

### 4.2 Memory leak check

- [x] Verify the dumper reports `0 mem alloc not freed` for every test run.
  - Fixed: `free_property()` in `tmx_mem.c` was not freeing `p->propertytype`
    (a `char*` field). Every property with a custom `propertytype` attribute
    leaked one allocation. Added `tmx_free_func(p->propertytype)` to
    `free_property()`.
- [x] Run under valgrind (if available): `valgrind --leak-check=full
      ./dumper b64zlib.tmx` — expect zero leaks and zero errors.
  - Result: `in use at exit: 0 bytes in 0 blocks`, `ERROR SUMMARY: 0 errors`
    for all test files.

### 4.3 API compatibility

- [x] `tmx.h` is byte-identical to upstream (no modifications).
- [x] C++ compatibility test: `c++ -I src/ -c test.cpp` compiles cleanly.
- [x] All function signatures in `tmx_utils.h` (the internal API) are
      preserved exactly, except for the removal of `setup_libxml_mem()`.
  - Done: guarded `setup_libxml_mem()` declaration with `#ifdef USE_LIBXML`
    so it is absent from yxml builds.

### 4.4 Edge cases

- [x] Verify error paths: corrupt XML produces clear error messages via
      `tmx_errno` / `tmx_strerr()`.
  - Result: `error: xml parser: failed to parse document`, 0 mem alloc not
    freed.
- [x] Verify empty layers, empty tilesets, empty object groups are handled.
  - Fixed: `mk_map_tile_array()` in `tmx_utils.c` crashed with a segfault
    (unsigned underflow at `tiles[tilecount - 1]`) when a tileset had
    `tilecount=0` and no image. Added an `else if (tilecount == 0)` guard
    so empty tilesets are handled without accessing the tiles array.
- [x] Verify self-closing XML elements (`<ellipse/>`, `<point/>`) parse
      correctly.
  - Result: `obj_type=ellipse` and `obj_type=point` appear correctly in
    dumper output for all relevant test files.
