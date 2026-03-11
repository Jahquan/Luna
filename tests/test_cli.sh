#!/bin/sh

set -eu

BIN=${1:-./luna}
ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
. "$ROOT_DIR/tests/expected_hashes.sh"

if [ ! -e "$BIN" ] && [ -f "$BIN.exe" ]; then
	BIN="$BIN.exe"
fi

sha256_file() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | awk '{print $1}'
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | awk '{print $1}'
	else
		echo "no SHA-256 tool found" >&2
		exit 1
	fi
}

python_cmd() {
	if command -v python3 >/dev/null 2>&1; then
		printf '%s\n' python3
	elif command -v python >/dev/null 2>&1; then
		printf '%s\n' python
	else
		echo "no Python interpreter found" >&2
		exit 1
	fi
}

copy_fixture() {
	src=$1
	dst=$2
	cp "$src" "$dst"
	"$(python_cmd)" - <<'PY' "$dst"
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
path.write_bytes(path.read_bytes().replace(b"\r\n", b"\n"))
PY
}

assert_file() {
	if [ ! -f "$1" ]; then
		echo "missing expected file: $1" >&2
		exit 1
	fi
}

assert_hash() {
	actual=$(sha256_file "$1")
	if [ "$actual" != "$2" ]; then
		echo "unexpected hash for $1" >&2
		echo "expected: $2" >&2
		echo "actual:   $actual" >&2
		exit 1
	fi
}

archive_manifest_hash() {
	"$(python_cmd)" "$ROOT_DIR/tests/tns_manifest.py" "$1"
}

assert_manifest_hash() {
	actual=$(archive_manifest_hash "$1")
	if [ "$actual" != "$2" ]; then
		echo "unexpected archive manifest hash for $1" >&2
		echo "expected: $2" >&2
		echo "actual:   $actual" >&2
		exit 1
	fi
}

assert_contains() {
	if ! strings "$1" | grep -Fq "$2"; then
		echo "expected to find '$2' in $1" >&2
		exit 1
	fi
}

assert_missing() {
	if [ -e "$1" ]; then
		echo "did not expect to find $1" >&2
		exit 1
	fi
}

assert_output_contains() {
	if ! printf '%s' "$1" | grep -Fq "$2"; then
		echo "expected output to contain '$2'" >&2
		exit 1
	fi
}

normalize_output() {
	printf '%s' "$1" | tr -d '\r'
}

TMP_BASE=${TMPDIR:-${TEMP:-${TMP:-/tmp}}}
if TMPDIR=$(mktemp -d 2>/dev/null); then
	:
else
	TMPDIR=$(mktemp -d "$TMP_BASE/luna-test.XXXXXX")
fi
trap 'rm -rf "$TMPDIR"' EXIT INT TERM

help_output=$("$BIN" --help)
help_output=$(normalize_output "$help_output")
assert_output_contains "$help_output" "usage:"
assert_output_contains "$help_output" "luna [INFILE.py]* [OUTFILE.tns]"
version_output=$("$BIN" --version)
version_output=$(normalize_output "$version_output")
[ "$version_output" = "Luna v2.1" ]

mkdir "$TMPDIR/explicit-python"
copy_fixture "$ROOT_DIR/tests/fixtures/main.py" "$TMPDIR/explicit-python/main.py"
copy_fixture "$ROOT_DIR/tests/fixtures/helper.py" "$TMPDIR/explicit-python/helper.py"
"$BIN" "$TMPDIR/explicit-python/main.py" "$TMPDIR/explicit-python/helper.py" "$TMPDIR/explicit-python/out.tns" >/dev/null
assert_file "$TMPDIR/explicit-python/out.tns"
assert_contains "$TMPDIR/explicit-python/out.tns" "Document.xml"
assert_contains "$TMPDIR/explicit-python/out.tns" "Problem1.xml"
assert_contains "$TMPDIR/explicit-python/out.tns" "main.py"
assert_contains "$TMPDIR/explicit-python/out.tns" "helper.py"
assert_manifest_hash "$TMPDIR/explicit-python/out.tns" "$MULTI_PY_MANIFEST_HASH"

mkdir "$TMPDIR/implicit-python"
copy_fixture "$ROOT_DIR/tests/fixtures/main.py" "$TMPDIR/implicit-python/main.py"
"$BIN" "$TMPDIR/implicit-python/main.py" >/dev/null
assert_file "$TMPDIR/implicit-python/main.tns"
assert_manifest_hash "$TMPDIR/implicit-python/main.tns" "$SINGLE_PY_MANIFEST_HASH"
"$BIN" "$TMPDIR/implicit-python/main.py" "$TMPDIR/implicit-python/explicit.tns" >/dev/null
cmp -s "$TMPDIR/implicit-python/main.tns" "$TMPDIR/implicit-python/explicit.tns"
rm -f "$ROOT_DIR/renamed.tns"
(cd "$ROOT_DIR" && "$BIN" "$TMPDIR/implicit-python/main.py" renamed.tns >/dev/null)
assert_file "$TMPDIR/implicit-python/renamed.tns"
assert_manifest_hash "$TMPDIR/implicit-python/renamed.tns" "$SINGLE_PY_MANIFEST_HASH"
if [ -f "$ROOT_DIR/renamed.tns" ]; then
	echo "single-file relative output should not be created in the current directory" >&2
	exit 1
fi

mkdir "$TMPDIR/spaced-filename"
copy_fixture "$ROOT_DIR/tests/fixtures/main.py" "$TMPDIR/spaced-filename/notes with spaces.py"
rm -f "$ROOT_DIR/space output.tns"
(cd "$ROOT_DIR" && "$BIN" "$TMPDIR/spaced-filename/notes with spaces.py" "space output.tns" >/dev/null)
assert_file "$TMPDIR/spaced-filename/space output.tns"
assert_missing "$ROOT_DIR/space output.tns"

mkdir "$TMPDIR/escaped-filename"
copy_fixture "$ROOT_DIR/tests/fixtures/main.py" "$TMPDIR/escaped-filename/amp&name space.py"
"$BIN" "$TMPDIR/escaped-filename/amp&name space.py" >/dev/null
assert_file "$TMPDIR/escaped-filename/amp&name space.tns"

mkdir -p "$TMPDIR/recursive-python/sub/deeper"
copy_fixture "$ROOT_DIR/tests/fixtures/main.py" "$TMPDIR/recursive-python/main.py"
copy_fixture "$ROOT_DIR/tests/fixtures/main.py" "$TMPDIR/recursive-python/sub/deeper/main.py"
printf 'ignore me\n' > "$TMPDIR/recursive-python/sub/deeper/notes.txt"
recursive_skipped=1
if ln -s "$TMPDIR/recursive-python/main.py" "$TMPDIR/recursive-python/sub/link.py" 2>/dev/null \
	&& ln -s "$TMPDIR/recursive-python" "$TMPDIR/recursive-python/sub/loop-dir" 2>/dev/null; then
	recursive_skipped=3
fi
recursive_output=$("$BIN" "$TMPDIR/recursive-python")
recursive_output=$(normalize_output "$recursive_output")
assert_file "$TMPDIR/recursive-python/main.tns"
assert_manifest_hash "$TMPDIR/recursive-python/main.tns" "$SINGLE_PY_MANIFEST_HASH"
assert_file "$TMPDIR/recursive-python/sub/deeper/main.tns"
assert_manifest_hash "$TMPDIR/recursive-python/sub/deeper/main.tns" "$SINGLE_PY_MANIFEST_HASH"
assert_missing "$TMPDIR/recursive-python/sub/deeper/notes.tns"
assert_output_contains "$recursive_output" "recursive conversion summary: converted 2, failed 0, skipped $recursive_skipped"
if [ "$recursive_skipped" -gt 1 ]; then
	assert_output_contains "$recursive_output" "[SKIP] skipping symlink"
	assert_missing "$TMPDIR/recursive-python/sub/link.tns"
fi
if "$BIN" "$TMPDIR/recursive-python" "$TMPDIR/recursive-python/out.tns" >/dev/null 2>&1; then
	echo "recursive directory mode should reject an explicit output path" >&2
	exit 1
fi
assert_missing "$TMPDIR/recursive-python/out.tns"

mkdir "$TMPDIR/empty-recursive"
if empty_output=$("$BIN" "$TMPDIR/empty-recursive" 2>&1); then
	echo "empty recursive directory should fail" >&2
	exit 1
fi
empty_output=$(normalize_output "$empty_output")
assert_output_contains "$empty_output" "recursive conversion summary: converted 0, failed 0, skipped 0"
assert_output_contains "$empty_output" "no Python files found to convert"

mkdir "$TMPDIR/resource"
"$(python_cmd)" - <<'PY' "$TMPDIR/resource/pixel.bmp"
import pathlib
import sys

pathlib.Path(sys.argv[1]).write_bytes(bytes.fromhex(
    "424d3a0000000000000036000000280000000100000001000000010018000000000004000000130b0000130b00000000000000000000ff000000"
))
PY
"$BIN" "$TMPDIR/resource/pixel.bmp" "$TMPDIR/resource/pixel.tns" >/dev/null
assert_file "$TMPDIR/resource/pixel.tns"
assert_contains "$TMPDIR/resource/pixel.tns" "*TIMLP0700"
assert_contains "$TMPDIR/resource/pixel.tns" "pixel.bmp"

mkdir "$TMPDIR/xml-bmp-bundle"
copy_fixture "$ROOT_DIR/tests/fixtures/Problem1.xml" "$TMPDIR/xml-bmp-bundle/Problem1.xml"
cp "$TMPDIR/resource/pixel.bmp" "$TMPDIR/xml-bmp-bundle/pixel.bmp"
"$BIN" "$TMPDIR/xml-bmp-bundle/Problem1.xml" "$TMPDIR/xml-bmp-bundle/pixel.bmp" "$TMPDIR/xml-bmp-bundle/bundle.tns" >/dev/null
assert_file "$TMPDIR/xml-bmp-bundle/bundle.tns"
assert_contains "$TMPDIR/xml-bmp-bundle/bundle.tns" "Document.xml"
assert_contains "$TMPDIR/xml-bmp-bundle/bundle.tns" "Problem1.xml"
assert_contains "$TMPDIR/xml-bmp-bundle/bundle.tns" "pixel.bmp"
assert_contains "$TMPDIR/xml-bmp-bundle/bundle.tns" "*TIMLP0700"

mkdir "$TMPDIR/problem"
copy_fixture "$ROOT_DIR/tests/fixtures/Problem1.xml" "$TMPDIR/problem/Problem1.xml"
"$BIN" "$TMPDIR/problem/Problem1.xml" "$TMPDIR/problem/problem.tns" >/dev/null
assert_file "$TMPDIR/problem/problem.tns"
assert_contains "$TMPDIR/problem/problem.tns" "Problem1.xml"
assert_manifest_hash "$TMPDIR/problem/problem.tns" "$PROBLEM_XML_MANIFEST_HASH"

mkdir "$TMPDIR/xml-comment"
cat <<'EOF' > "$TMPDIR/xml-comment/Problem1.xml"
<?xml version="1.0" encoding="UTF-8"?>
<prob xmlns="urn:TI.Problem" ver="1.0" pbname=""><!-- keep comment --><sym><expr>1+1</expr></sym></prob>
EOF
"$BIN" "$TMPDIR/xml-comment/Problem1.xml" "$TMPDIR/xml-comment/comment.tns" >/dev/null
assert_file "$TMPDIR/xml-comment/comment.tns"

mkdir "$TMPDIR/xml-self-closing"
cat <<'EOF' > "$TMPDIR/xml-self-closing/Problem1.xml"
<?xml version="1.0" encoding="UTF-8"?>
<prob xmlns="urn:TI.Problem" ver="1.0" pbname=""><sym><expr/></sym></prob>
EOF
"$BIN" "$TMPDIR/xml-self-closing/Problem1.xml" "$TMPDIR/xml-self-closing/self-closing.tns" >/dev/null
assert_file "$TMPDIR/xml-self-closing/self-closing.tns"

mkdir "$TMPDIR/xml-pi"
cat <<'EOF' > "$TMPDIR/xml-pi/Problem1.xml"
<?xml version="1.0" encoding="UTF-8"?>
<prob xmlns="urn:TI.Problem" ver="1.0" pbname=""><sym><?calc keep="1"?><expr>1+1</expr></sym></prob>
EOF
"$BIN" "$TMPDIR/xml-pi/Problem1.xml" "$TMPDIR/xml-pi/pi.tns" >/dev/null
assert_file "$TMPDIR/xml-pi/pi.tns"

mkdir "$TMPDIR/xml-quoted-gt"
cat <<'EOF' > "$TMPDIR/xml-quoted-gt/Problem1.xml"
<?xml version="1.0" encoding="UTF-8"?>
<prob xmlns="urn:TI.Problem" ver="1.0" pbname=""><sym><expr test="1>0">1+1</expr></sym></prob>
EOF
"$BIN" "$TMPDIR/xml-quoted-gt/Problem1.xml" "$TMPDIR/xml-quoted-gt/quoted-gt.tns" >/dev/null
assert_file "$TMPDIR/xml-quoted-gt/quoted-gt.tns"

mkdir "$TMPDIR/xml-invalid"
cat <<'EOF' > "$TMPDIR/xml-invalid/Problem1.xml"
<?xml version="1.0" encoding="UTF-8"?>
<prob xmlns="urn:TI.Problem" ver="1.0" pbname=""><sym><expr>1+1</sym></expr></prob>
EOF
cp "$TMPDIR/problem/problem.tns" "$TMPDIR/xml-invalid/invalid.tns"
if invalid_output=$("$BIN" "$TMPDIR/xml-invalid/Problem1.xml" "$TMPDIR/xml-invalid/invalid.tns" 2>&1); then
	echo "malformed XML should fail conversion" >&2
	exit 1
fi
invalid_output=$(normalize_output "$invalid_output")
assert_output_contains "$invalid_output" "$TMPDIR/xml-invalid/Problem1.xml"
assert_output_contains "$invalid_output" "line 2 column"
assert_manifest_hash "$TMPDIR/xml-invalid/invalid.tns" "$PROBLEM_XML_MANIFEST_HASH"

mkdir "$TMPDIR/xml-doctype"
cat <<'EOF' > "$TMPDIR/xml-doctype/Problem1.xml"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE prob>
<prob xmlns="urn:TI.Problem" ver="1.0" pbname=""><sym><expr>1+1</expr></sym></prob>
EOF
if doctype_output=$("$BIN" "$TMPDIR/xml-doctype/Problem1.xml" "$TMPDIR/xml-doctype/doctype.tns" 2>&1); then
	echo "DOCTYPE XML should fail conversion" >&2
	exit 1
fi
doctype_output=$(normalize_output "$doctype_output")
assert_output_contains "$doctype_output" "DOCTYPE is not supported"
assert_missing "$TMPDIR/xml-doctype/doctype.tns"

mkdir "$TMPDIR/stdin-lua"
printf 'print("stdin lua")\n' | "$BIN" - "$TMPDIR/stdin-lua/stdin.tns" >/dev/null
assert_file "$TMPDIR/stdin-lua/stdin.tns"
assert_manifest_hash "$TMPDIR/stdin-lua/stdin.tns" "$STDIN_LUA_MANIFEST_HASH"

if printf 'print("stdin lua")\n' | "$BIN" - >/dev/null 2>&1; then
	echo "stdin conversion without an explicit output path should fail" >&2
	exit 1
fi
assert_missing "$ROOT_DIR/stdin.tns"

echo "ok"
