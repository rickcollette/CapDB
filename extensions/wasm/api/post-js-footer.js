/*
  2022-07-22

  The author disclaims copyright to this source code.  In place of a
  legal notice, here is a blessing:

  *   May you do good and not evil.
  *   May you find forgiveness for yourself and forgive others.
  *   May you share freely, never taking more than you give.

  ***********************************************************************

  This file is the tail end of the capdb-api.js constellation,
  closing the function scope opened by post-js-header.js.

  In terms of amalgamation code placement, this file is appended
  immediately after the final capdb-api-*.js piece. Those files
  cooperate to prepare capdbApiBootstrap() and this file calls it.
  It is run within a context which gives it access to Emscripten's
  Module object, after capdb.wasm is loaded but before
  capdbApiBootstrap() has been called.

  Because this code resides (after building) inside the function
  installed by post-js-header.js, it has access to state set up by
  pre-js.c-pp.js and friends.
*/
try{
  /* We are in the closing block of Module.runSQLite3PostLoadInit(), so
     its arguments are visible here. */

  /* Config options for capdbApiBootstrap(). */
  const bootstrapConfig = Object.assign(
    Object.create(null),
    /** The WASM-environment-dependent configuration for capdbApiBootstrap() */
    {
      memory: ('undefined'!==typeof wasmMemory)
        ? wasmMemory
        : EmscriptenModule['wasmMemory'],
      exports: ('undefined'!==typeof wasmExports)
        ? wasmExports /* emscripten >=3.1.44 */
        : (Object.prototype.hasOwnProperty.call(EmscriptenModule,'wasmExports')
           ? EmscriptenModule['wasmExports']
           : EmscriptenModule['asm']/* emscripten <=3.1.43 */)
    },
    globalThis.capdbApiBootstrap.defaultConfig, // default options
    globalThis.capdbApiConfig || {} // optional client-provided options
  );

  capdbInitScriptInfo.debugModule("Bootstrapping lib config", bootstrapConfig);

  /**
     For purposes of the Emscripten build, call capdbApiBootstrap().
     Ideally clients should be able to inject their own config here,
     but that's not practical in this particular build constellation
     because of the order everything happens in.  Clients may either
     define globalThis.capdbApiConfig or modify
     globalThis.capdbApiBootstrap.defaultConfig to tweak the default
     configuration used by a no-args call to capdbApiBootstrap(),
     but must have first loaded their WASM module in order to be able
     to provide the necessary configuration state.
  */
  const p = globalThis.capdbApiBootstrap(bootstrapConfig);
  delete globalThis.capdbApiBootstrap;
  return p /* the eventual result of globalThis.capdbInitModule() */;
}catch(e){
  console.error("capdbApiBootstrap() error:",e);
  throw e;
}

//console.warn("This is the end of the Module.runSQLite3PostLoadInit handler.");
}/*Module.runSQLite3PostLoadInit(...)*/;
//console.warn("This is the end of the setup of the (pending) Module.runSQLite3PostLoadInit");
