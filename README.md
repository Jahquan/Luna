Luna 2.1
========

Luna is a portable command-line converter of Lua and Python scripts to TNS TI-Nspire documents.  
Lua scripts require OS 3.0.2 or later, and Python scripts require CX II OS 5.2 or later.

It can also be used to convert any TI-Nspire problems in XML format to TNS documents.

## Project status

This repository is a maintained fork of the original [ndless-nspire/Luna](https://github.com/ndless-nspire/Luna) project.
The goal of this fork is to preserve Luna's core TNS conversion behavior while modernizing the project around it:

* keep the original converter and TI-Nspire format behavior intact
* make the CLI safer and less surprising
* add regression tests so changes are verifiable
* keep a working Emscripten/browser build
* document the project as it exists now, not as it existed years ago

## What this fork adds

This fork keeps Luna's core conversion behavior intact while modernizing the project around it:

* safer output handling with temp-file writes and final-path reporting
* same-directory output for single-file conversions, even when you pass a bare `OUTFILE.tns`
* recursive Python directory conversion with deterministic traversal and symlink skipping
* improved XML parsing and diagnostics via a vendored Expat backend
* regression tests and sanitizer runs for native builds
* a richer browser UI for the Emscripten build, including folder upload and drag-and-drop

## Modernization summary

### Converter and CLI

* Added `--help` and `--version`
* Single-file conversions now default to writing the `.tns` beside the source file
* A bare explicit output name like `notes.tns` in single-file mode is also resolved beside the source file
* Successful conversions print the final output path that was written
* Output writes now go through a temp file before final replacement
* Recursive directory mode converts every `.py` file in place and prints a converted/failed/skipped summary
* Recursive traversal is deterministic and skips symlink loops

### XML and Python metadata handling

* Replaced the ad hoc XML parser with a vendored Expat-backed parser
* Added support for XML comments, self-closing tags, and processing instructions in the conversion path
* XML failures now report clearer line and column information
* Embedded Python filenames are escaped and Unicode-safe before insertion into generated XML metadata

### Tooling and verification

* Added a real regression suite with fixtures and stable output hashes
* Added `make test` and `make sanitize`
* Vendored Expat into `third_party/expat`
* Cleaned remaining web-build warnings so the Emscripten build is clean

### Web build

* Added multi-file bundle mode for `.lua`, `.py`, `.xml`, and `.bmp`
* Added recursive Python folder conversion in the browser
* Added drag-and-drop for files and supported folders
* Added an in-page run log, selection previews, status cards, and download list
* Verified the browser bundle builds with `emcc`

## Features

* Convert `.lua` programs into `.tns` documents
* Convert one or more `.py` files into `.tns` documents
* Convert TI-Nspire XML problem/document bundles into `.tns`
* Pack `.bmp` resources into XML document bundles
* Recursively convert Python folder trees in place
* Build and run a browser version through Emscripten
* Verify output stability with regression hashes and sanitizer builds

## Usage

* Single-file conversion:    `./luna INFILE.py` or `./luna Problem1.xml`
* Single-file with name:     `./luna INFILE.py OUTFILE.tns`
* Lua program conversion:    `./luna INFILE.lua OUTFILE.tns`
* Problem conversion:        `./luna Problem1.xml OUTFILE.tns`
* Multiple files:            `./luna Document.xml Problem1.xml [Problem2.xml...] OUTFILE.tns`
* Python conversion:         `./luna InFile1.py [InFile2.py...] OUTFILE.tns`
* Recursive Python folders:  `./luna worksheets/`
* Help / version:            `./luna --help`, `./luna --version`

If the input is `-`, it reads the file from the standard input.  
If you run Luna from the repo root after `make`, use `./luna ...`. If you install the binary on your `PATH`, you can drop the `./`.  
If you only pass a single input file, Luna writes the `.tns` next to that source file. If you pass a single input plus a bare output filename like `notes.tns`, that output filename is also created next to the source file.  
If any input path is a directory, Luna recursively converts every `.py` file it finds and writes each `.tns` next to its source file. Recursive traversal is deterministic, prints a converted/failed/skipped summary, and skips symlinked files/directories to avoid loops.  
Make sure to encode your Lua or Problem file in UTF-8 if it contains special characters. You can also pack arbitrary files like images into the TNS.  
For Python, the first script will be the one that shows when the TNS document is opened.
On success, Luna prints the final output path it wrote.

## Examples

```sh
# Single Python file, output beside the source file
./luna ~/Desktop/notes.py

# Single Python file, explicit name still written beside the source file
./luna ~/Desktop/notes.py notes.tns

# Multiple Python files into one document
./luna main.py helper.py lesson.tns

# Recursive in-place Python conversion
./luna worksheets/

# XML problem/document bundle with a BMP resource
./luna Document.xml Problem1.xml image.bmp worksheet.tns

# Build and serve the browser UI locally
source ~/Downloads/emsdk/emsdk_env.sh
make -C emscripten
cd emscripten
python3 -m http.server 8000
```

## Bugs, feedback...

Please use the GitHub [issue tracker](https://github.com/Jahquan/Luna/issues).

## License

Luna is licensed under the Mozilla Public License v1.1.  
Luna is based on a derived version of MiniZip. See minizip-1.1/MiniZip64_info.txt for more information.  
Luna includes cryptographic software written by Eric Young (eay@cryptsoft.com), see the DES.* files for more information.

## Building it yourself

You need the zlib (zlib1g-dev/zlib-devel) development library. On macOS, you can use [brew](http://brew.sh/): `brew install zlib`  
Then you can just run `make`. The locally built binary is `./luna`.  
The XML parser is vendored from Expat 2.7.4 under `third_party/expat`, so there is no extra system XML dependency to install.

Run `make test` to execute the regression checks.
Run `make sanitize` to exercise the converter under AddressSanitizer and UBSan.
Run `make -C emscripten` to build the browser bundle when `emcc` is installed. The web UI supports bundling selected files into one `.tns`, uploading a folder to recursively convert every `.py` file in place, drag-and-drop for files/folders, and an in-page run log plus download list.

On macOS, if `emcc` comes from a home-directory `emsdk` install, activate it first:

```sh
source ~/Downloads/emsdk/emsdk_env.sh
```

## Web UI

After building the Emscripten target, serve the `emscripten/` directory as your web root:

```sh
make -C emscripten
cd emscripten
python3 -m http.server 8000
```

Then open `http://localhost:8000/luna.html`.

The browser UI supports:

* bundling loose `.lua`, `.py`, `.xml`, and `.bmp` files into one `.tns`
* recursive folder conversion for Python trees
* drag-and-drop for loose files and supported folders
* an in-page stdout/stderr log from Luna
* a download list for generated `.tns` files

## Verification

Native verification is driven by [tests/test_cli.sh](tests/test_cli.sh), which exercises:

* single-file and multi-file Python conversion
* same-directory output placement rules
* recursive directory conversion
* BMP resource packing
* XML comments, self-closing tags, processing instructions, and failure paths
* stdin Lua conversion
* malformed XML and non-clobbering failure behavior

This fork does not use CI/CD workflows. Verification is expected to be run locally before changes are committed or published.

Recommended local verification commands:

```sh
make
make test
make sanitize
source ~/Downloads/emsdk/emsdk_env.sh
make -C emscripten
```

## History

2022-09-29: v2.1
 * NEW: Support for CDATA
 * FIX: luna now defaults to an older file header version unless one of the input files is a .bmp
 * FIX: Fix invalid generation of XML documents with more than 256 tags
 * FIX: Fix crash when using with XML documents with many tags

2016-12-28: v2.0
 * NEW: No OpenSSL library dependency anymore, just use the relevant DES code.
 * FIX: Fix crash when parsing deeply nested XML documents
 
2016-12-27: v1.0.1
 * NEW: Show Luna version in usage message
 * FIX: Some minor warnings

2016-12-27: v1.0
 * NEW: Install target in Makefile
 * NEW: Add support for .BMP resources
 * NEW: License under Mozilla Public License v1.1
 * FIX: Fix build with OpenSSL 1.1 using newer DES API

2012-06-26: v0.3a
 * FIX: build options for Mac/Linux
 * FIX: skip UTF-8 BOM if any
 * FIX: compatibility with some Unicode characters such as Chinese ones

2012-06-24: v0.3
 * FIX: support all UTF-8 characters
 * NEW: Lua program produced now default to OS v3.2's Lua API level 2 (see
        https://wiki.inspired-lua.org/Changes_in_OS_3.2).

2011-11-10: v0.2b
 * FIX: buffer overflow

2011-09-27: v0.2a
 * FIX: characters at the end of a Lua script might no be correctly XML-escaped
        (thanks Goplat)
 * FIX: wrong buffer size handling might cause '<' and '&' characters in Lua
        scripts to produce bad TNS files (thanks Goplat)
 * FIX: TNS documents were abnormally big because of trailing garbage data being
        encrypted (thanks Levak)

2011-09-19: v0.2
 * NEW: supports arbitrary TI-Nspire problem conversion, useful to build
        third-party document generators

2011-08-08: v0.1b
 * NEW: lua file can be provided from stdin
 * FIX: crash if input file doesn't exist

2011-08-06: v0.1a
 * Can be built on Linux

2011-08-05: v0.1
 * First release
