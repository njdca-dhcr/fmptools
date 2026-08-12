[![Build Status](https://github.com/evanmiller/fmptools/workflows/build/badge.svg)](https://github.com/evanmiller/fmptools/actions)

FMP Tools
--

Some tools for reading FileMaker Pro files (fp3, fp5, fp7, and fmp12). See the
included [HACKING](./HACKING) file for technical information on the FileMaker
format.

Building from the git source first requires [autoconf](https://www.gnu.org/software/autoconf/):

```
autoreconf -i -f
```

Building from a release requires the usual:

```
./configure
make
make install
```

The tools installed to `$PREFIX/bin` include:

* `fmp2excel` - Convert a FileMaker Pro database to Excel (requires [libxlsxwriter](http://libxlsxwriter.github.io))
* `fmp2json` - Convert a FileMaker Pro database to JSON (requires [yajl](https://lloyd.github.io/yajl/))
* `fmp2sqlite` - Convert a FileMaker Pro database to SQLite (requires [sqlite](https://www.sqlite.org/index.html))

There is also a C library installed that is used by the above tools, but the
API is subject to change.

Large-file memory behavior
--

The `v0.2.3-dca.2` fork release bounds parser memory during file-backed
conversions in two ways:

* File payloads are read through a private, read-only `mmap` instead of being
  copied into the heap when the file is opened.
* Parsed chunk chains are released after each block is processed instead of
  being cached for the lifetime of the open file.

File-backed callers must keep the input file unchanged until `fmp_close_file`
returns. Buffer-backed callers retain the previous copy-owning behavior. The
parser still builds an in-memory block index and still scans the block chain
once per table, so very large files require memory proportional to their block
count and conversion time grows with the number of tables.

The SQLite converter skips FileMaker helper tables that have no columns;
SQLite does not permit a zero-column `CREATE TABLE` statement.

You might also enjoy [fp5dump](https://github.com/qwesda/fp5dump), although
that project does not read the newer fp7 and fmp12 formats.
