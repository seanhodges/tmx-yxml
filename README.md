# TMX C Loader

---

> **Note**
> This is an experimental fork of the main tmx library, please see https://github.com/baylej/tmx/.

![Build and Test](https://github.com/baylej/tmx/workflows/Build%20and%20Test/badge.svg)

## About

A portable C library to load [tiled](http://mapeditor.org) maps in your games.

## Dependencies

This project depends on (optional) [Zlib](http://zlib.net/) with two backends:
```
cmake -B build -D WANT_LIBXML=Off (use vendored YXML)
cmake -B build -D WANT_LIBXML=On ([LibXml2](http://xmlsoft.org) is compulsory).
```

## Usage

**Please read the [Documentation](http://libtmx.rtfd.io/).**
