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
