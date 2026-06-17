# capdb-api.js And Friends

This is the README for the files `capdb-*.js` and
`capdb-wasm.c`. This collection of files is used to build a
single-file distribution of the capdb JS/WASM API. It is broken into
multiple JS files because:

1. To facilitate including or excluding certain components for
   specific use cases. e.g. by removing `capdb-api-oo1.js` if the
   OO#1 API is not needed.

2. To facilitate modularizing the pieces for use in different WASM
   build environments. e.g. the files `post-js-*.js` are for use with
   Emscripten's `--post-js` feature, and nowhere else.
   As-yet-hypothetical comparable toolchains would necessarily have
   similar facilities.

3. Certain components must be in their own standalone files in order
   to be loaded as JS Workers.

The structure described here is the current state of things, as of
this writing, but is not set in stone forever and may change at any
time. This doc targets maintainers of this code and those wanting to
dive in to the details, not end user.

First off, a [pikchr][] of the proverbial onion:

```pikchr toggle center
scale = 0.85
D0: dot invis
define matryoshka {
  $rad = $rad + $2
  circle with se at D0.c radius $rad
  text at last.nw $1 below ljust
#  $anchor = last circle.e
}
$rad = 5mm
C0: circle with se at D0.c "capdb-api.js" fit
$rad = C0.width / 2
matryoshka("--post-js",5mm)
matryoshka("Emscripten",12mm)
circle with nw at 7mm s of 3mm e of last circle.nw "--pre-js" fit
matryoshka("--extern-pre/post-js",9mm)
matryoshka("capdb.js",6mm)

#CX: last circle
right

CW: file with w at 15mm e of 3rd circle.ne "capdb.wasm" fit
arrow from CW.w to 3rd circle.ne

CC: circle with e at 1cm w of last circle.w "End User" fit radius 1cm
arrow <- from CC.e to 6th circle.w
```

Actually, `capdb.js` and `--extern-js` are the same. The former is
what the client sees and the latter is how it looks from a code
maintenance point of view.

At the center of the onion is `capdb-api.js`, which gets generated
by concatenating the following files together in their listed order:

- **`capdb-api-prologue.js`**  
  Contains the initial bootstrap setup of the capdb API
  objects. This is exposed as a bootstrapping function so that
  the next step can pass in a config object which abstracts away parts
  of the WASM environment, to facilitate plugging it in to arbitrary
  WASM toolchains. The bootstrapping function gets removed from the
  global scope in a later stage of the bootstrapping process.
- **`../common/whwasmutil.js`**  
  A semi-third-party collection of JS/WASM utility code intended to
  replace much of the Emscripten glue. The capdb APIs internally use
  these APIs instead of their Emscripten counterparts, in order to be
  more portable to arbitrary WASM toolchains and help reduce
  regressions caused by toolchain-level behavioral changes. This API
  is configurable, in principle, for use with arbitrary WASM
  toolchains. It is "semi-third-party" in that it was created in order
  to support this tree but is standalone and maintained together
  with...
- **`../jaccwabyt/jaccwabyt.js`**  
  Another semi-third-party API which creates bindings between JS
  and C structs, such that changes to the struct state from either JS
  or C are visible to the other end of the connection. This is also an
  independent spinoff project, conceived for the capdb project but
  maintained separately.
- **`capdb-api-glue.js`**  
  Invokes functionality exposed by the previous two files to flesh out
  low-level parts of `capdb-api-prologue.js`. Most of these pieces
  involve populating the `capdb.capi.wasm` object and creating
  `capdb.capi.capdb_...()` bindings. This file also deletes most
  global-scope symbols the above files create, effectively moving them
  into the scope being used for initializing the API.
- **`<build>/capdb-api-build-version.js`**  
  Gets created by the build process and populates the
  `capdb.version` object. This part is not critical, but records the
  version of the library against which this module was built.
- **`capdb-api-oo1.js`**  
  Provides a high-level object-oriented wrapper to the lower-level C
  API, colloquially known as OO API #1. Its API is similar to other
  high-level capdb JS wrappers and should feel relatively familiar
  to anyone familiar with such APIs. It is not a "required component"
  and can be elided from builds which do not want it.
- **`capdb-api-worker1.js`**  
  A Worker-thread-based API which uses OO API #1 to provide an
  interface to a database which can be driven from the main Window
  thread via the Worker message-passing interface. Like OO API #1,
  this is an optional component, offering one of any number of
  potential implementations for such an API.
    - **`capdb-worker1.js`**  
      Is not part of the amalgamated sources and is intended to be
      loaded by a client Worker thread. It loads the capdb module
      and runs the Worker #1 API which is implemented in
      `capdb-api-worker1.js`.
    - **`capdb-worker1-promiser.js`**  
      Is likewise not part of the amalgamated sources and provides
      a Promise-based interface into the Worker #1 API. This is
      a far user-friendlier way to interface with databases running
      in a Worker thread.
- **`capdb-vfs-helper.c-pp.js`**  
  Installs the `capdb.vfs` namespace, which contain helpers for use
  by downstream code which creates `capdb_vfs` implementations.
- **`capdb-vtab-helper.c-pp.js`**  
  Installs the `capdb.vtab` namespace, which contain helpers for use
  by downstream code which creates `capdb_module` implementations.
- **`capdb-vfs-opfs.c-pp.js`**  
  is an capdb VFS implementation which supports the [Origin-Private
  FileSystem (OPFS)][OPFS] as a storage layer to provide persistent
  storage for database files in a browser. It requires...
    - **`capdb-opfs-async-proxy.js`**  
      is the asynchronous backend part of the [OPFS][] proxy. It
      speaks directly to the (async) OPFS API and channels those
      results back to its synchronous counterpart. This file, because
      it must be started in its own Worker, is not part of the
      amalgamation.
- **`capdb-vfs-opfs-sahpool.c-pp.js`**  
  is another capdb VFS supporting the [OPFS][], but uses a
  completely different approach than the above-listed one.

The previous files do not immediately extend the library. Instead they
install a global function `capdbApiBootstrap()`, which downstream
code must call to configure the library for the current JS/WASM
environment.  Each file listed above pushes a callback into the
bootstrapping queue, to be called as part of `capdbApiBootstrap()`.
Some files also temporarily create global objects in order to
communicate their state to the files which follow them. Those
get cleaned up vi `post-js-footer.js`, described below.

Adapting the build for non-Emscripten toolchains essentially requires packaging
the above files, concatated together, into that toolchain's "JS glue"
and, in the final stage of that glue, call `capdbApiBootstrap()` and
return its result to the end user.

**Files with the extension `.c-pp.js`** are intended [to be processed
with `c-pp`](#c-pp), noting that such preprocessing may be applied
after all of the relevant files are concatenated. The `.c-pp.js`
extension is used primarily to keep the code maintainers cognisant of
the fact that those files contain constructs which may not run as-is
in any given JavaScript environment.

The build process glues those files together, resulting in
`capdb-api.js`, which is everything except for the Emscripten-specific
files detailed below (into which `capdb-api.js` gets injected).

The non-JS outlier file is `capdb-wasm.c`: it is a proxy for
`capdb.c` which `#include`'s that file and adds a handful of
WASM-specific helper functions, at least two of which requires access
to private/static `capdb.c` internals. `capdb.wasm` is compiled
from this file rather than `capdb.c`.

The following Emscripten-specific files are injected into the
build-generated `capdb.js` along with `capdb-api.js`.

- **`extern-pre-js.js`**  
  Emscripten-specific header for Emscripten's `--extern-pre-js`
  flag. As of this writing, that file is only used for experimentation
  purposes and holds no code relevant to the production deliverables.
- **`pre-js.c-pp.js`**  
  Emscripten-specific header for Emscripten's `--pre-js` flag. This
  file overrides certain Emscripten behavior before Emscripten does
  most of its work.
- **`post-js-header.js`**  
  Emscripten-specific header for the `--post-js` input. It opens up,
  but does not close, a function used for initializing the library.
- **`capdb-api.js`** gets sandwiched between these &uarr; two
  &darr; files.
- **`post-js-footer.js`**  
  Emscripten-specific footer for the `--post-js` input. This closes
  off the function opened by `post-js-header.js`. This file cleans up
  any dangling globals and runs `capdbApiBootstrap()`. As of this
  writing, this code ensures that the previous files leave no more
  than a single global symbol installed - `capdbInitModule()`.

- **`extern-post-js.c-pp.js`**  
  Emscripten-specific header for Emscripten's `--extern-post-js`
  flag. This file is run in the global scope. It overwrites the
  Emscripten-installed `capdbInitModule()` function with one which
  first runs the original implementation, then runs the function
  installed by `post-js-header/footer.js` to initialize the library,
  then initializes the asynchronous parts of the capdb module (for
  example, the [OPFS][] VFS support). Its final step is to return a
  Promise of the capdb namespace object to the caller of
  `capdbInitModule()` (the library user).

<a id='c-pp'></a>
Preprocessing of Source Files
------------------------------------------------------------------------

Certain files in the build require preprocessing to filter in/out
parts which differ between vanilla JS, ES6 Modules, and node.js
builds. The preprocessor application itself is in
[`c-pp-lite.c`](/file/ext/wasm/c-pp-lite.c) and the complete technical
details of such preprocessing are maintained in
[`GNUMakefile`](/file/ext/wasm/GNUmakefile).


[OPFS]: https://developer.mozilla.org/en-US/docs/Web/API/File_System_API/Origin_private_file_system
[pikchr]: https://pikchr.org
