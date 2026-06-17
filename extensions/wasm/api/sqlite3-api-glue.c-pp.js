/*
  2022-07-22

  The author disclaims copyright to this source code.  In place of a
  legal notice, here is a blessing:

  *   May you do good and not evil.
  *   May you find forgiveness for yourself and forgive others.
  *   May you share freely, never taking more than you give.

  ***********************************************************************

  This file glues together disparate pieces of JS which are loaded in
  previous steps of the capdb-api.js bootstrapping process:
  capdb-api-prologue.js, whwasmutil.js, and jaccwabyt.js. It
  initializes the main API pieces so that the downstream components
  (e.g. capdb-api-oo1.js) have all of the infrastructure that they
  need.
*/
globalThis.capdbApiBootstrap.initializers.push(function(capdb){
  'use strict';
  const toss = (...args)=>{throw new Error(args.join(' '))};
  const capi = capdb.capi, wasm = capdb.wasm, util = capdb.util;
  globalThis.WhWasmUtilInstaller(wasm);
  delete globalThis.WhWasmUtilInstaller;

  if(0){
    /**
       Please keep this block around as a maintenance reminder
       that we cannot rely on this type of check.

       This block fails on Safari, per a report at
       https://sqlite.org/forum/forumpost/e5b20e1feb.

       It turns out that what Safari serves from the indirect function
       table (e.g. wasm.functionEntry(X)) is anonymous functions which
       wrap the WASM functions, rather than returning the WASM
       functions themselves. That means comparison of such functions
       is useless for determining whether or not we have a specific
       function from wasm.exports. i.e. if function X is indirection
       function table entry N then wasm.exports.X is not equal to
       wasm.functionEntry(N) in Safari, despite being so in the other
       browsers.
    */
    /**
       Find a mapping for CAPDB_WASM_DEALLOC, which the API
       guarantees is a WASM pointer to the same underlying function as
       wasm.dealloc() (noting that wasm.dealloc() is permitted to be a
       JS wrapper around the WASM function). There is unfortunately no
       O(1) algorithm for finding this pointer: we have to walk the
       WASM indirect function table to find it. However, experience
       indicates that that particular function is always very close to
       the front of the table (it's been entry #3 in all relevant
       tests).
    */
    const dealloc = wasm.exports[capdb.config.deallocExportName];
    const nFunc = wasm.functionTable().length;
    let i;
    for(i = 0; i < nFunc; ++i){
      const e = wasm.functionEntry(i);
      if(dealloc === e){
        capi.CAPDB_WASM_DEALLOC = i;
        break;
      }
    }
    if(dealloc !== wasm.functionEntry(capi.CAPDB_WASM_DEALLOC)){
      toss("Internal error: cannot find function pointer for CAPDB_WASM_DEALLOC.");
    }
  }

  /**
     Signatures for the WASM-exported C-side functions. Each entry
     is an array with 2+ elements:

     [ "c-side name",
       "result type" (wasm.xWrap() syntax),
       [arg types in xWrap() syntax]
       // ^^^ this needn't strictly be an array: it can be subsequent
       // elements instead: [x,y,z] is equivalent to x,y,z
     ]

     Support for the API-specific data types in the result/argument
     type strings gets plugged in at a later phase in the API
     initialization process.
  */
  const bindingSignatures = {
    core: [
      // Please keep these sorted by function name!
      ["capdb_aggregate_context","void*", "capdb_context*", "int"],
      /* capdb_auto_extension() has a hand-written binding. */
      /* capdb_bind_blob() and capdb_bind_text() have hand-written
         bindings to permit more flexible inputs. */
      ["capdb_bind_double","int", "capdb_stmt*", "int", "f64"],
      ["capdb_bind_int","int", "capdb_stmt*", "int", "int"],
      ["capdb_bind_null",undefined, "capdb_stmt*", "int"],
      ["capdb_bind_parameter_count", "int", "capdb_stmt*"],
      ["capdb_bind_parameter_index","int", "capdb_stmt*", "string"],
      ["capdb_bind_parameter_name", "string", "capdb_stmt*", "int"],
      ["capdb_bind_pointer", "int",
       "capdb_stmt*", "int", "*", "string:static", "*"],
      /* sqlite_bind_text() is hand-written */
      ["capdb_bind_zeroblob", "int", "capdb_stmt*", "int", "int"],
      ["capdb_busy_handler","int", [
        "capdb*",
        new wasm.xWrap.FuncPtrAdapter({
          signature: 'i(pi)',
          contextKey: (argv,argIndex)=>argv[0/* capdb* */]
        }),
        "*"
      ]],
      ["capdb_busy_timeout","int", "capdb*", "int"],
      /* capdb_cancel_auto_extension() has a hand-written binding. */
      /* capdb_close_v2() is implemented by hand to perform some
         extra work. */
      ["capdb_changes", "int", "capdb*"],
      ["capdb_clear_bindings","int", "capdb_stmt*"],
      ["capdb_collation_needed", "int", "capdb*", "*", "*"/*=>v(ppis)*/],
      ["capdb_column_blob","*", "capdb_stmt*", "int"],
      ["capdb_column_bytes","int", "capdb_stmt*", "int"],
      ["capdb_column_count", "int", "capdb_stmt*"],
      ["capdb_column_decltype", "string", "capdb_stmt*", "int"],
      ["capdb_column_double","f64", "capdb_stmt*", "int"],
      ["capdb_column_int","int", "capdb_stmt*", "int"],
      ["capdb_column_name","string", "capdb_stmt*", "int"],
//#define proxy-text-apis 1
//#if not proxy-text-apis
/* Search this file for tag:proxy-text-apis to see what this is about. */
      ["capdb_column_text","string", "capdb_stmt*", "int"],
//#/if
      ["capdb_column_type","int", "capdb_stmt*", "int"],
      ["capdb_column_value","capdb_value*", "capdb_stmt*", "int"],
      ["capdb_commit_hook", "void*", [
        "capdb*",
        new wasm.xWrap.FuncPtrAdapter({
          name: 'capdb_commit_hook',
          signature: 'i(p)',
          contextKey: (argv)=>argv[0/* capdb* */]
        }),
        '*'
      ]],
      ["capdb_compileoption_get", "string", "int"],
      ["capdb_compileoption_used", "int", "string"],
      ["capdb_complete", "int", "string:flexible"],
      ["capdb_context_db_handle", "capdb*", "capdb_context*"],
      /* capdb_create_collation() and capdb_create_collation_v2()
         use hand-written bindings to simplify passing of the callback
         function. */
      /* capdb_create_function(), capdb_create_function_v2(), and
         capdb_create_window_function() use hand-written bindings to
         simplify handling of their function-type arguments. */
      ["capdb_data_count", "int", "capdb_stmt*"],
      ["capdb_db_filename", "string", "capdb*", "string"],
      ["capdb_db_handle", "capdb*", "capdb_stmt*"],
      ["capdb_db_name", "string", "capdb*", "int"],
      ["capdb_db_readonly", "int", "capdb*", "string"],
      ["capdb_db_status", "int", "capdb*", "int", "*", "*", "int"],
      ["capdb_errcode", "int", "capdb*"],
      ["capdb_errmsg", "string", "capdb*"],
      ["capdb_error_offset", "int", "capdb*"],
      ["capdb_errstr", "string", "int"],
      ["capdb_exec", "int", [
        "capdb*", "string:flexible",
        new wasm.xWrap.FuncPtrAdapter({
          signature: 'i(pipp)',
          bindScope: 'transient',
          callProxy: (callback)=>{
            let aNames;
            return (pVoid, nCols, pColVals, pColNames)=>{
              try {
                const aVals = wasm.cArgvToJs(nCols, pColVals);
                if(!aNames) aNames = wasm.cArgvToJs(nCols, pColNames);
                return callback(aVals, aNames) | 0;
              }catch(e){
                /* If we set the db error state here, the higher-level
                   exec() call replaces it with its own, so we have no way
                   of reporting the exception message except the console. We
                   must not propagate exceptions through the C API. Though
                   we make an effort to report OOM here, capdb_exec()
                   translates that into CAPDB_ABORT as well. */
                return e.resultCode || capi.CAPDB_ERROR;
              }
            }
          }
        }),
        "*", "**"
      ]],
      ["capdb_expanded_sql", "string", "capdb_stmt*"],
      ["capdb_extended_errcode", "int", "capdb*"],
      ["capdb_extended_result_codes", "int", "capdb*", "int"],
      ["capdb_file_control", "int", "capdb*", "string", "int", "*"],
      ["capdb_finalize", "int", "capdb_stmt*"],
      ["capdb_free", undefined,"*"],
      ["capdb_get_autocommit", "int", "capdb*"],
      ["capdb_get_auxdata", "*", "capdb_context*", "int"],
      ["capdb_initialize", undefined],
      ["capdb_interrupt", undefined, "capdb*"],
      ["capdb_is_interrupted", "int", "capdb*"],
      ["capdb_keyword_count", "int"],
      ["capdb_keyword_name", "int", ["int", "**", "*"]],
      ["capdb_keyword_check", "int", ["string", "int"]],
      ["capdb_libversion", "string"],
      ["capdb_libversion_number", "int"],
      ["capdb_limit", "int", ["capdb*", "int", "int"]],
      ["capdb_malloc", "*","int"],
      ["capdb_next_stmt", "capdb_stmt*", ["capdb*","capdb_stmt*"]],
      ["capdb_open", "int", "string", "*"],
      ["capdb_open_v2", "int", "string", "*", "int", "string"],
      /* capdb_prepare_v2() and capdb_prepare_v3() are handled
         separately due to us requiring two different sets of semantics
         for those, depending on how their SQL argument is provided. */
      /* capdb_randomness() uses a hand-written wrapper to extend
         the range of supported argument types. */
      ["capdb_realloc", "*","*","int"],
      ["capdb_reset", "int", "capdb_stmt*"],
      /* capdb_reset_auto_extension() has a hand-written binding. */
      ["capdb_result_blob", undefined, "capdb_context*", "*", "int", "*"],
      ["capdb_result_double", undefined, "capdb_context*", "f64"],
      ["capdb_result_error", undefined, "capdb_context*", "string", "int"],
      ["capdb_result_error_code", undefined, "capdb_context*", "int"],
      ["capdb_result_error_nomem", undefined, "capdb_context*"],
      ["capdb_result_error_toobig", undefined, "capdb_context*"],
      ["capdb_result_int", undefined, "capdb_context*", "int"],
      ["capdb_result_null", undefined, "capdb_context*"],
      ["capdb_result_pointer", undefined,
       "capdb_context*", "*", "string:static", "*"],
      ["capdb_result_subtype", undefined, "capdb_value*", "int"],
      ["capdb_result_text", undefined, "capdb_context*", "string", "int", "*"],
      ["capdb_result_zeroblob", undefined, "capdb_context*", "int"],
      ["capdb_rollback_hook", "void*", [
        "capdb*",
        new wasm.xWrap.FuncPtrAdapter({
          name: 'capdb_rollback_hook',
          signature: 'v(p)',
          contextKey: (argv)=>argv[0/* capdb* */]
        }),
        '*'
      ]],
      /**
         We do not have a way to automatically clean up destructors
         which are automatically converted from JS functions via the
         final argument to capdb_set_auxdata(). Because of that,
         automatic function conversion is not supported for this
         function.  Clients should use wasm.installFunction() to create
         such callbacks, then pass that pointer to
         capdb_set_auxdata(). Relying on automated conversions here
         would lead to leaks of JS/WASM proxy functions because
         capdb_set_auxdata() is frequently called in UDFs.

         The capdb.oo1.DB class's onclose handlers can be used for this
         purpose. For example:

         const pAuxDtor = wasm.installFunction('v(p)', function(ptr){
           //free ptr
         });
         myDb.onclose = {
           after: ()=>{
             wasm.uninstallFunction(pAuxDtor);
           }
         };

         Then pass pAuxDtor as the final argument to appropriate
         capdb_set_auxdata() calls.

         Prior to 3.49.0 this binding ostensibly had automatic
         function conversion here but a typo prevented it from
         working.  Rather than fix it, it was removed because testing
         the fix brought the huge potential for memory leaks to the
         forefront.
      */
      ["capdb_set_auxdata", undefined, [
        "capdb_context*", "int", "*",
        true
          ? "*"
          : new wasm.xWrap.FuncPtrAdapter({
            /* If we can find a way to automate their cleanup, JS functions can
               be auto-converted with this. */
            name: 'xDestroyAuxData',
            signature: 'v(p)',
            contextKey: (argv, argIndex)=>argv[0/* capdb_context* */]
          })
      ]],
      ['capdb_set_errmsg', 'int', 'capdb*', 'int', 'string'],
      ["capdb_shutdown", undefined],
      ["capdb_sourceid", "string"],
      ["capdb_sql", "string", "capdb_stmt*"],
      ["capdb_status", "int", "int", "*", "*", "int"],
      ["capdb_step", "int", "capdb_stmt*"],
      ["capdb_stmt_busy", "int", "capdb_stmt*"],
      ["capdb_stmt_readonly", "int", "capdb_stmt*"],
      ["capdb_stmt_status", "int", "capdb_stmt*", "int", "int"],
      ["capdb_strglob", "int", "string","string"],
      ["capdb_stricmp", "int", "string", "string"],
      ["capdb_strlike", "int", "string", "string","int"],
      ["capdb_strnicmp", "int", "string", "string", "int"],
      ["capdb_table_column_metadata", "int",
       "capdb*", "string", "string", "string",
       "**", "**", "*", "*", "*"],
      ["capdb_total_changes", "int", "capdb*"],
      ["capdb_trace_v2", "int", [
        "capdb*", "int",
        new wasm.xWrap.FuncPtrAdapter({
          name: 'capdb_trace_v2::callback',
          signature: 'i(ippp)',
          contextKey: (argv,argIndex)=>argv[0/* capdb* */]
        }),
        "*"
      ]],
      ["capdb_txn_state", "int", ["capdb*","string"]],
      /* capdb_uri_...() have very specific requirements for their
         first C-string arguments, so we cannot perform any
         string-type conversion on their first argument. */
      ["capdb_uri_boolean", "int", "capdb_filename", "string", "int"],
      ["capdb_uri_key", "string", "capdb_filename", "int"],
      ["capdb_uri_parameter", "string", "capdb_filename", "string"],
      ["capdb_user_data","void*", "capdb_context*"],
      ["capdb_value_blob", "*", "capdb_value*"],
      ["capdb_value_bytes","int", "capdb_value*"],
      ["capdb_value_double","f64", "capdb_value*"],
      ["capdb_value_dup", "capdb_value*", "capdb_value*"],
      ["capdb_value_free", undefined, "capdb_value*"],
      ["capdb_value_frombind", "int", "capdb_value*"],
      ["capdb_value_int","int", "capdb_value*"],
      ["capdb_value_nochange", "int", "capdb_value*"],
      ["capdb_value_numeric_type", "int", "capdb_value*"],
      ["capdb_value_pointer", "*", "capdb_value*", "string:static"],
      ["capdb_value_subtype", "int", "capdb_value*"],
//#if not proxy-text-apis
      ["capdb_value_text", "string", "capdb_value*"],
//#/if
      ["capdb_value_type", "int", "capdb_value*"],
      ["capdb_vfs_find", "*", "string"],
      ["capdb_vfs_register", "int", "capdb_vfs*", "int"],
      ["capdb_vfs_unregister", "int", "capdb_vfs*"]

      /* This list gets extended with optional pieces below */
    ]/*.core*/,
    /**
       Functions which require BigInt (int64) support are separated
       from the others because we need to conditionally bind them or
       apply dummy impls, depending on the capabilities of the
       environment.  (That said: we never actually build without
       BigInt support, and such builds are untested.)

       Not all of these functions directly require int64 but are only
       for use with APIs which require int64. For example, the
       vtab-related functions.
    */
    int64: [
      ["capdb_bind_int64","int", ["capdb_stmt*", "int", "i64"]],
      ["capdb_changes64","i64", ["capdb*"]],
      ["capdb_column_int64","i64", ["capdb_stmt*", "int"]],
      ["capdb_deserialize", "int", "capdb*", "string", "*", "i64", "i64", "int"]
      /* Careful! Short version: de/serialize() are problematic because they
         might use a different allocator than the user for managing the
         deserialized block. de/serialize() are ONLY safe to use with
         capdb_malloc(), capdb_free(), and its 64-bit variants. Because
         of this, the canonical builds of capdb.wasm/js guarantee that
         capdb.wasm.alloc() and friends use those allocators. Custom builds
         may not guarantee that, however. */,
      ["capdb_last_insert_rowid", "i64", ["capdb*"]],
      ["capdb_malloc64", "*","i64"],
      ["capdb_msize", "i64", "*"],
      ["capdb_overload_function", "int", ["capdb*","string","int"]],
      ["capdb_realloc64", "*","*", "i64"],
      ["capdb_result_int64", undefined, "*", "i64"],
      ["capdb_result_zeroblob64", "int", "*", "i64"],
      ["capdb_serialize","*", "capdb*", "string", "*", "int"],
      ["capdb_set_last_insert_rowid", undefined, ["capdb*", "i64"]],
      ["capdb_status64", "int", "int", "*", "*", "int"],
      ["capdb_db_status64", "int", "capdb*", "int", "*", "*", "int"],
      ["capdb_total_changes64", "i64", ["capdb*"]],
      ["capdb_update_hook", "*", [
        "capdb*",
        new wasm.xWrap.FuncPtrAdapter({
          name: 'capdb_update_hook::callback',
          signature: "v(pippj)",
          contextKey: (argv)=>argv[0/* capdb* */],
          callProxy: (callback)=>{
            return (p,op,z0,z1,rowid)=>{
              callback(p, op, wasm.cstrToJs(z0), wasm.cstrToJs(z1), rowid);
            };
          }
        }),
        "*"
      ]],
      ["capdb_uri_int64", "i64", ["capdb_filename", "string", "i64"]],
      ["capdb_value_int64","i64", "capdb_value*"]
      /* This list gets extended with optional pieces below */
    ]/*.int64*/,
    /**
       Functions which are intended solely for API-internal use by the
       WASM components, not client code. These get installed into
       capdb.util. Some of them get exposed to clients via variants
       in capdb_js_...().

       2024-01-11: these were renamed, with two underscores in the
       prefix, to ensure that clients do not accidentally depend on
       them.  They have always been documented as internal-use-only,
       so no clients "should" be depending on the old names.
    */
    wasmInternal: [
      ["capdb__wasm_db_reset", "int", "capdb*"],
      ["capdb__wasm_db_vfs", "capdb_vfs*", "capdb*","string"],
      [/* DO NOT USE. This is deprecated since 2023-08-11 because it
          can trigger assert() in debug builds when used with file
          sizes which are not an exact multiple of a valid db page
          size.  This function is retained only so that
          capdb_js_vfs_create_file() can continue to work (for a
          given value of work), but that function emits a
          config.warn() log message directing the reader to
          alternatives. */
        "capdb__wasm_vfs_create_file", "int", "capdb_vfs*","string","*", "int"
      ],
      ["capdb__wasm_posix_create_file", "int", "string","*", "int"],
      ["capdb__wasm_vfs_unlink", "int", "capdb_vfs*","string"],
      ["capdb__wasm_qfmt_token","string:dealloc", "string","int"]
    ]/*.wasmInternal*/
  } /*bindingSignatures*/;

  if( !!wasm.exports.capdb_progress_handler ){
    bindingSignatures.core.push(
      ["capdb_progress_handler", undefined, [
        "capdb*", "int", new wasm.xWrap.FuncPtrAdapter({
          name: 'xProgressHandler',
          signature: 'i(p)',
          bindScope: 'context',
          contextKey: (argv,argIndex)=>argv[0/* capdb* */]
        }), "*"
      ]]
    );
  }

  if( !!wasm.exports.capdb_stmt_explain ){
    bindingSignatures.core.push(
      ["capdb_stmt_explain", "int", "capdb_stmt*", "int"],
      ["capdb_stmt_isexplain", "int", "capdb_stmt*"]
    );
  }

  if( !!wasm.exports.capdb_set_authorizer ){
    bindingSignatures.core.push(
      ["capdb_set_authorizer", "int", [
        "capdb*",
        new wasm.xWrap.FuncPtrAdapter({
          name: "capdb_set_authorizer::xAuth",
          signature: "i(pi"+"ssss)",
          contextKey: (argv, argIndex)=>argv[0/*(capdb*)*/],
          /**
             We use callProxy here to ensure (A) that exceptions
             thrown from callback() have well-defined behavior and (B)
             that its result is coerced to an integer.
          */
          callProxy: (callback)=>{
            return (pV, iCode, s0, s1, s2, s3)=>{
              try{
                s0 = s0 && wasm.cstrToJs(s0); s1 = s1 && wasm.cstrToJs(s1);
                s2 = s2 && wasm.cstrToJs(s2); s3 = s3 && wasm.cstrToJs(s3);
                return callback(pV, iCode, s0, s1, s2, s3) | 0;
              }catch(e){
                return e.resultCode || capi.CAPDB_ERROR;
              }
            }
          }
        }),
        "*"/*pUserData*/
      ]]
    );
  }/* capdb_set_authorizer() */

  if( !!wasm.exports.capdb_column_origin_name ){
    bindingSignatures.core.push(
      ["capdb_column_database_name","string", "capdb_stmt*", "int"],
      ["capdb_column_origin_name","string", "capdb_stmt*", "int"],
      ["capdb_column_table_name","string", "capdb_stmt*", "int"]
    );
  }

  if(false && wasm.compileOptionUsed('CAPDB_ENABLE_NORMALIZE')){
    /* ^^^ "the problem" is that this is an optional feature and the
       build-time function-export list does not currently take
       optional features into account. */
    bindingSignatures.core.push(["capdb_normalized_sql", "string", "capdb_stmt*"]);
  }

//#if enable-see
  if( !!wasm.exports.capdb_key_v2 ){
    /**
       This code is capable of using an SEE build but an SEE WASM
       build is generally incompatible with SEE's license conditions.
       The project's official stance on WASM builds of SEE is: it is
       permitted for use internally within organizations which have
       licensed SEE, but not for sites made available to the public or
       to unlicensed end users because exposing an SEE build of
       capdb.wasm effectively provides all clients with a working
       copy of SEE.
    */
    bindingSignatures.core.push(
      ["capdb_key", "int", "capdb*", "string", "int"],
      ["capdb_key_v2","int","capdb*","string","*","int"],
      ["capdb_rekey", "int", "capdb*", "string", "int"],
      ["capdb_rekey_v2", "int", "capdb*", "string", "*", "int"],
      ["capdb_activate_see", undefined, "string"]
    );
  }
//#/if enable-see

  if( wasm.bigIntEnabled && !!wasm.exports.capdb_declare_vtab ){
    bindingSignatures.int64.push(
      ["capdb_create_module", "int",
       ["capdb*","string","capdb_module*","*"]],
      ["capdb_create_module_v2", "int",
       ["capdb*","string","capdb_module*","*","*"]],
      ["capdb_declare_vtab", "int", ["capdb*", "string:flexible"]],
      ["capdb_drop_modules", "int", ["capdb*", "**"]],
      ["capdb_vtab_collation","string","capdb_index_info*","int"],
      /*["capdb_vtab_config" is variadic and requires a hand-written
        proxy.] */
      ["capdb_vtab_distinct","int", "capdb_index_info*"],
      ["capdb_vtab_in","int", "capdb_index_info*", "int", "int"],
      ["capdb_vtab_in_first", "int", "capdb_value*", "**"],
      ["capdb_vtab_in_next", "int", "capdb_value*", "**"],
      ["capdb_vtab_nochange","int", "capdb_context*"],
      ["capdb_vtab_on_conflict","int", "capdb*"],
      ["capdb_vtab_rhs_value","int", "capdb_index_info*", "int", "**"]
    );
  }/* virtual table APIs */

  if(wasm.bigIntEnabled && !!wasm.exports.capdb_preupdate_hook){
    bindingSignatures.int64.push(
      ["capdb_preupdate_blobwrite", "int", "capdb*"],
      ["capdb_preupdate_count", "int", "capdb*"],
      ["capdb_preupdate_depth", "int", "capdb*"],
      ["capdb_preupdate_hook", "*", [
        "capdb*",
        new wasm.xWrap.FuncPtrAdapter({
          name: 'capdb_preupdate_hook',
          signature: "v(ppippjj)",
          contextKey: (argv)=>argv[0/* capdb* */],
          callProxy: (callback)=>{
            return (p,db,op,zDb,zTbl,iKey1,iKey2)=>{
              callback(p, db, op, wasm.cstrToJs(zDb), wasm.cstrToJs(zTbl),
                       iKey1, iKey2);
            };
          }
        }),
        "*"
      ]],
      ["capdb_preupdate_new", "int", ["capdb*", "int", "**"]],
      ["capdb_preupdate_old", "int", ["capdb*", "int", "**"]]
    );
  } /* preupdate API */

  // Add session/changeset APIs...
  if(wasm.bigIntEnabled
     && !!wasm.exports.capdb_changegroup_add
     && !!wasm.exports.capdb_session_create
     && !!wasm.exports.capdb_preupdate_hook /* required by the session API */){
    /**
       FuncPtrAdapter options for session-related callbacks with the
       native signature "i(ps)". This proxy converts the 2nd argument
       from a C string to a JS string before passing the arguments on
       to the client-provided JS callback.
    */
    const __ipsProxy = {
      signature: 'i(ps)',
      callProxy:(callback)=>{
        return (p,s)=>{
          try{return callback(p, wasm.cstrToJs(s)) | 0}
          catch(e){return e.resultCode || capi.CAPDB_ERROR}
        }
      }
    };

    bindingSignatures.int64.push(
      ['capdb_changegroup_add', 'int', ['capdb_changegroup*', 'int', 'void*']],
      ['capdb_changegroup_add_strm', 'int', [
        'capdb_changegroup*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xInput', signature: 'i(ppp)', bindScope: 'transient'
        }),
        'void*'
      ]],
      ['capdb_changegroup_delete', undefined, ['capdb_changegroup*']],
      ['capdb_changegroup_new', 'int', ['**']],
      ['capdb_changegroup_output', 'int', ['capdb_changegroup*', 'int*', '**']],
      ['capdb_changegroup_output_strm', 'int', [
        'capdb_changegroup*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xOutput', signature: 'i(ppi)', bindScope: 'transient'
        }),
        'void*'
      ]],
      ['capdb_changeset_apply', 'int', [
        'capdb*', 'int', 'void*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xFilter', bindScope: 'transient', ...__ipsProxy
        }),
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xConflict', signature: 'i(pip)', bindScope: 'transient'
        }),
        'void*'
      ]],
      ['capdb_changeset_apply_strm', 'int', [
        'capdb*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xInput', signature: 'i(ppp)', bindScope: 'transient'
        }),
        'void*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xFilter', bindScope: 'transient', ...__ipsProxy
        }),
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xConflict', signature: 'i(pip)', bindScope: 'transient'
        }),
        'void*'
      ]],
      ['capdb_changeset_apply_v2', 'int', [
        'capdb*', 'int', 'void*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xFilter', bindScope: 'transient', ...__ipsProxy
        }),
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xConflict', signature: 'i(pip)', bindScope: 'transient'
        }),
        'void*', '**', 'int*', 'int'

      ]],
      ['capdb_changeset_apply_v2_strm', 'int', [
        'capdb*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xInput', signature: 'i(ppp)', bindScope: 'transient'
        }),
        'void*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xFilter', bindScope: 'transient', ...__ipsProxy
        }),
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xConflict', signature: 'i(pip)', bindScope: 'transient'
        }),
        'void*', '**', 'int*', 'int'
      ]],
      ['capdb_changeset_apply_v3', 'int', [
        'capdb*', 'int', 'void*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xFilter', signature: 'i(pp)', bindScope: 'transient'
        }),
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xConflict', signature: 'i(pip)', bindScope: 'transient'
        }),
        'void*', '**', 'int*', 'int'

      ]],
      ['capdb_changeset_apply_v3_strm', 'int', [
        'capdb*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xInput', signature: 'i(ppp)', bindScope: 'transient'
        }),
        'void*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xFilter', signature: 'i(pp)', bindScope: 'transient'
        }),
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xConflict', signature: 'i(pip)', bindScope: 'transient'
        }),
        'void*', '**', 'int*', 'int'
      ]],
      ['capdb_changeset_concat', 'int', ['int','void*', 'int', 'void*', 'int*', '**']],
      ['capdb_changeset_concat_strm', 'int', [
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xInputA', signature: 'i(ppp)', bindScope: 'transient'
        }),
        'void*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xInputB', signature: 'i(ppp)', bindScope: 'transient'
        }),
        'void*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xOutput', signature: 'i(ppi)', bindScope: 'transient'
        }),
        'void*'
      ]],
      ['capdb_changeset_conflict', 'int', ['capdb_changeset_iter*', 'int', '**']],
      ['capdb_changeset_finalize', 'int', ['capdb_changeset_iter*']],
      ['capdb_changeset_fk_conflicts', 'int', ['capdb_changeset_iter*', 'int*']],
      ['capdb_changeset_invert', 'int', ['int', 'void*', 'int*', '**']],
      ['capdb_changeset_invert_strm', 'int', [
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xInput', signature: 'i(ppp)', bindScope: 'transient'
        }),
        'void*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xOutput', signature: 'i(ppi)', bindScope: 'transient'
        }),
        'void*'
      ]],
      ['capdb_changeset_new', 'int', ['capdb_changeset_iter*', 'int', '**']],
      ['capdb_changeset_next', 'int', ['capdb_changeset_iter*']],
      ['capdb_changeset_old', 'int', ['capdb_changeset_iter*', 'int', '**']],
      ['capdb_changeset_op', 'int', [
        'capdb_changeset_iter*', '**', 'int*', 'int*','int*'
      ]],
      ['capdb_changeset_pk', 'int', ['capdb_changeset_iter*', '**', 'int*']],
      ['capdb_changeset_start', 'int', ['**', 'int', '*']],
      ['capdb_changeset_start_strm', 'int', [
        '**',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xInput', signature: 'i(ppp)', bindScope: 'transient'
        }),
        'void*'
      ]],
      ['capdb_changeset_start_v2', 'int', ['**', 'int', '*', 'int']],
      ['capdb_changeset_start_v2_strm', 'int', [
        '**',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xInput', signature: 'i(ppp)', bindScope: 'transient'
        }),
        'void*', 'int'
      ]],
      ['capdb_session_attach', 'int', ['capdb_session*', 'string']],
      ['capdb_session_changeset', 'int', ['capdb_session*', 'int*', '**']],
      ['capdb_session_changeset_size', 'i64', ['capdb_session*']],
      ['capdb_session_changeset_strm', 'int', [
        'capdb_session*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xOutput', signature: 'i(ppp)', bindScope: 'transient'
        }),
        'void*'
      ]],
      ['capdb_session_config', 'int', ['int', 'void*']],
      ['capdb_session_create', 'int', ['capdb*', 'string', '**']],
      //capdb_session_delete() is bound manually
      ['capdb_session_diff', 'int', ['capdb_session*', 'string', 'string', '**']],
      ['capdb_session_enable', 'int', ['capdb_session*', 'int']],
      ['capdb_session_indirect', 'int', ['capdb_session*', 'int']],
      ['capdb_session_isempty', 'int', ['capdb_session*']],
      ['capdb_session_memory_used', 'i64', ['capdb_session*']],
      ['capdb_session_object_config', 'int', ['capdb_session*', 'int', 'void*']],
      ['capdb_session_patchset', 'int', ['capdb_session*', '*', '**']],
      ['capdb_session_patchset_strm', 'int', [
        'capdb_session*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xOutput', signature: 'i(ppp)', bindScope: 'transient'
        }),
        'void*'
      ]],
      ['capdb_session_table_filter', undefined, [
        'capdb_session*',
        new wasm.xWrap.FuncPtrAdapter({
          name: 'xFilter', ...__ipsProxy,
          contextKey: (argv,argIndex)=>argv[0/* (capdb_session*) */]
        }),
        '*'
      ]]
    );
  }/*session/changeset APIs*/

  /**
     Prepare JS<->C struct bindings for the non-opaque struct types we
     need...
  */
  capdb.StructBinder = globalThis.Jaccwabyt({
    heap: wasm.heap8u,
    alloc: wasm.alloc,
    dealloc: wasm.dealloc,
    bigIntEnabled: wasm.bigIntEnabled,
    pointerIR: wasm.ptr.ir,
    memberPrefix: /* Never change this: this prefix is baked into any
                     amount of code and client-facing docs. (Much
                     later: it probably should have been '$$', but see
                     the previous sentence.) */ '$'
  });
  delete globalThis.Jaccwabyt;

  {// wasm.xWrap() bindings...

    /* Convert Arrays and certain TypedArrays to strings for
       'string:flexible'-type arguments */
    const __xString = wasm.xWrap.argAdapter('string');
    wasm.xWrap.argAdapter(
      'string:flexible', (v)=>__xString(util.flexibleString(v))
    );

    /**
       The 'string:static' argument adapter treats its argument as
       either...

       - WASM pointer: assumed to be a long-lived C-string which gets
         returned as-is.

       - Anything else: gets coerced to a JS string for use as a map
         key. If a matching entry is found (as described next), it is
         returned, else wasm.allocCString() is used to create a a new
         string, map its pointer to a copy of (''+v) for the remainder
         of the application's life, and returns that pointer value for
         this call and all future calls which are passed a
         string-equivalent argument.

       Use case: capdb_bind_pointer(), capdb_result_pointer(), and
       capdb_value_pointer() call for "a static string and
       preferably a string literal". This converter is used to ensure
       that the string value seen by those functions is long-lived and
       behaves as they need it to, at the cost of a one-time leak of
       each distinct key.
    */
    wasm.xWrap.argAdapter(
      'string:static',
      function(v){
        if(wasm.isPtr(v)) return v;
        v = ''+v;
        let rc = this[v];
        return rc || (this[v] = wasm.allocCString(v));
      }.bind(Object.create(null))
    );

    /**
       Add some descriptive xWrap() aliases for '*' intended to (A)
       improve readability/correctness of bindingSignatures and (B)
       provide automatic conversion from higher-level representations,
       e.g. capi.capdb_vfs to `capdb_vfs*` via (capi.capdb_vfs
       instance).pointer.
    */
    const __xArgPtr = wasm.xWrap.argAdapter('*');
    const nilType = function(){
      /*a class which no value can ever be an instance of*/
    };
    wasm.xWrap.argAdapter('capdb_filename', __xArgPtr)
    ('capdb_context*', __xArgPtr)
    ('capdb_value*', __xArgPtr)
    ('void*', __xArgPtr)
    ('capdb_changegroup*', __xArgPtr)
    ('capdb_changeset_iter*', __xArgPtr)
    ('capdb_session*', __xArgPtr)
    ('capdb_stmt*', (v)=>
      __xArgPtr((v instanceof (capdb?.oo1?.Stmt || nilType))
           ? v.pointer : v))
    ('capdb*', (v)=>
      __xArgPtr((v instanceof (capdb?.oo1?.DB || nilType))
           ? v.pointer : v))
    /**
       `capdb_vfs*`:

       - v is-a string: use the result of capdb_vfs_find(v) but
         throw if it returns 0.
       - v is-a capi.capdb_vfs: use v.pointer.
       - Else return the same as the `'*'` argument conversion.
    */
    ('capdb_vfs*', (v)=>{
      if('string'===typeof v){
        /* A NULL capdb_vfs pointer will be treated as the default
           VFS in many contexts. We specifically do not want that
           behavior here. */
        return capi.capdb_vfs_find(v)
          || capdb.SQLite3Error.toss(
            capi.CAPDB_NOTFOUND,
            "Unknown capdb_vfs name:", v
          );
      }
      return __xArgPtr((v instanceof (capi.capdb_vfs || nilType))
                       ? v.pointer : v);
    });
    if( wasm.exports.capdb_declare_vtab ){
      wasm.xWrap.argAdapter('capdb_index_info*', (v)=>
        __xArgPtr((v instanceof (capi.capdb_index_info || nilType))
                  ? v.pointer : v))
      ('capdb_module*', (v)=>
        __xArgPtr((v instanceof (capi.capdb_module || nilType))
                  ? v.pointer : v)
      );
    }

    /**
       Alias `T*` to `*` for return type conversions for common T
       types, primarily to improve legibility of their binding
       signatures.
    */
    const __xRcPtr = wasm.xWrap.resultAdapter('*');
    wasm.xWrap.resultAdapter('capdb*', __xRcPtr)
    ('capdb_context*', __xRcPtr)
    ('capdb_stmt*', __xRcPtr)
    ('capdb_value*', __xRcPtr)
    ('capdb_vfs*', __xRcPtr)
    ('void*', __xRcPtr);

    /**
       Populate api object with capdb_...() by binding the "raw" wasm
       exports into type-converting proxies using wasm.xWrap().
    */
    for(const e of bindingSignatures.core){
      capi[e[0]] = wasm.xWrap.apply(null, e);
    }
    for(const e of bindingSignatures.wasmInternal){
      util[e[0]] = wasm.xWrap.apply(null, e);
    }

    /* For C API functions which cannot work properly unless
       wasm.bigIntEnabled is true, install a bogus impl which throws
       if called when bigIntEnabled is false. The alternative would be
       to elide these functions altogether, which seems likely to
       cause more confusion, as documented APIs would resolve to the
       undefined value in incompatible builds. */
    for(const e of bindingSignatures.int64){
      capi[e[0]] = wasm.bigIntEnabled
        ? wasm.xWrap.apply(null, e)
        : ()=>toss(e[0]+"() is unavailable due to lack",
                   "of BigInt support in this build.");
    }

    /* We're done with bindingSignatures but it's const, so... */
    delete bindingSignatures.core;
    delete bindingSignatures.int64;
    delete bindingSignatures.wasmInternal;

    /**
       Sets the given db's error state. Accepts:

       - (capdb*, int code, string msg)
       - (capdb*, Error e [,string msg = ''+e])

       If passed a WasmAllocError, the message is ignored and the
       result code is CAPDB_NOMEM. If passed any other Error type,
       the result code defaults to CAPDB_ERROR unless the Error
       object has a resultCode property, in which case that is used
       (e.g. SQLite3Error has that). If passed a non-WasmAllocError
       exception, the message string defaults to ''+theError.

       Returns either the final result code, capi.CAPDB_NOMEM if
       setting the message string triggers an OOM, or
       capi.CAPDB_MISUSE if pDb is NULL or invalid (with the caveat
       that behavior in the later case is undefined if pDb is not
       "valid enough").

       Pass (pDb,0,0) to clear the error state.
    */
    util.capdb__wasm_db_error = function(pDb, resultCode, message){
      if( !pDb ) return capi.CAPDB_MISUSE;
      if(resultCode instanceof capdb.WasmAllocError){
        resultCode = capi.CAPDB_NOMEM;
        message = 0 /*avoid allocating message string*/;
      }else if(resultCode instanceof Error){
        message = message || ''+resultCode;
        resultCode = (resultCode.resultCode || capi.CAPDB_ERROR);
      }
      return capi.capdb_set_errmsg(pDb, resultCode, message) || resultCode;
    };
  }/*xWrap() bindings*/

  {/* Import C-level constants and structs... */
    const cJson = wasm.xCall('capdb__wasm_enum_json');
    if(!cJson){
      toss("Maintenance required: increase capdb__wasm_enum_json()'s",
           "static buffer size!");
    }
    wasm.ctype = JSON.parse(wasm.cstrToJs(cJson));
    // Groups of CAPDB_xyz macros...
    const defineGroups = ['access', 'authorizer',
                          'blobFinalizers', 'changeset',
                          'config', 'dataTypes',
                          'dbConfig', 'dbStatus',
                          'encodings', 'fcntl', 'flock', 'ioCap',
                          'limits', 'openFlags',
                          'prepareFlags', 'resultCodes',
                          'capdbStatus',
                          'stmtStatus', 'syncFlags',
                          'trace', 'txnState', 'udfFlags',
                          'version'];
    if(wasm.bigIntEnabled){
      defineGroups.push('serialize', 'session', 'vtab');
    }
    for(const t of defineGroups){
      for(const e of Object.entries(wasm.ctype[t])){
        // ^^^ [k,v] there triggers a buggy code transformation via
        // one of the Emscripten-driven optimizers.
        capi[e[0]] = e[1];
      }
    }
    if(!wasm.functionEntry(capi.CAPDB_WASM_DEALLOC)){
      toss("Internal error: cannot resolve exported function",
           "entry CAPDB_WASM_DEALLOC (=="+capi.CAPDB_WASM_DEALLOC+").");
    }
    const __rcMap = Object.create(null);
    for(const e of Object.entries(wasm.ctype['resultCodes'])){
      __rcMap[e[1]] = e[0];
    }
    /**
       For the given integer, returns the CAPDB_xxx result code as a
       string, or undefined if no such mapping is found.
    */
    capi.capdb_js_rc_str = (rc)=>__rcMap[rc];

    /* Bind all registered C-side structs... */
    const notThese = Object.assign(Object.create(null),{
      // For each struct to NOT register, map its name to true:
      WasmTestStruct: true,
      /* capdb_index_info and friends require int64: */
      capdb_index_info: !wasm.bigIntEnabled,
      capdb_index_constraint: !wasm.bigIntEnabled,
      capdb_index_orderby: !wasm.bigIntEnabled,
      capdb_index_constraint_usage: !wasm.bigIntEnabled
    });
    for(const s of wasm.ctype.structs){
      if(!notThese[s.name]){
        capi[s.name] = capdb.StructBinder(s);
      }
    }
    if(capi.capdb_index_info){
      /* Move these inner structs into capdb_index_info.  Binding
      ** them to WASM requires that we create top-level structs to
      ** model them with, but those are no longer needed after we've
      ** passed them to StructBinder. */
      for(const k of ['capdb_index_constraint',
                      'capdb_index_orderby',
                      'capdb_index_constraint_usage']){
        capi.capdb_index_info[k] = capi[k];
        delete capi[k];
      }
      capi.capdb_vtab_config = wasm.xWrap(
        'capdb__wasm_vtab_config','int',[
          'capdb*', 'int', 'int']
      );
    }/* end vtab-related setup */
  }/*end C constant and struct imports*/

  /**
     Internal helper to assist in validating call argument counts in
     the hand-written capdb_xyz() wrappers. We do this only for
     consistency with non-special-case wrappings.
  */
  const __dbArgcMismatch = (pDb,f,n)=>{
    return util.capdb__wasm_db_error(pDb, capi.CAPDB_MISUSE,
                                      f+"() requires "+n+" argument"+
                                      (1===n?"":'s')+".");
  };

  /** Code duplication reducer for functions which take an encoding
      argument and require CAPDB_UTF8.  Sets the db error code to
      CAPDB_FORMAT, installs a descriptive error message,
      and returns CAPDB_FORMAT. */
  const __errEncoding = (pDb)=>{
    return util.capdb__wasm_db_error(
      pDb, capi.CAPDB_FORMAT, "CAPDB_UTF8 is the only supported encoding."
    );
  };

  /**
     __dbCleanupMap is infrastructure for recording registration of
     UDFs and collations so that capdb_close_v2() can clean up any
     automated JS-to-WASM function conversions installed by those.
  */
  const __argPDb = (pDb)=>wasm.xWrap.argAdapter('capdb*')(pDb);
  const __argStr = (str)=>wasm.isPtr(str) ? wasm.cstrToJs(str) : str;
  const __dbCleanupMap = function(
    pDb, mode/*0=remove, >0=create if needed, <0=do not create if missing*/
  ){
    pDb = __argPDb(pDb);
    let m = this.dbMap.get(pDb);
    if(!mode){
      this.dbMap.delete(pDb);
      return m;
    }else if(!m && mode>0){
      this.dbMap.set(pDb, (m = Object.create(null)));
    }
    return m;
  }.bind(Object.assign(Object.create(null),{
    dbMap: new Map
  }));

  __dbCleanupMap.addCollation = function(pDb, name){
    const m = __dbCleanupMap(pDb, 1);
    if(!m.collation) m.collation = new Set;
    m.collation.add(__argStr(name).toLowerCase());
  };

  __dbCleanupMap._addUDF = function(pDb, name, arity, map){
    /* Map UDF name to a Set of arity values */
    name = __argStr(name).toLowerCase();
    let u = map.get(name);
    if(!u) map.set(name, (u = new Set));
    u.add((arity<0) ? -1 : arity);
  };

  __dbCleanupMap.addFunction = function(pDb, name, arity){
    const m = __dbCleanupMap(pDb, 1);
    if(!m.udf) m.udf = new Map;
    this._addUDF(pDb, name, arity, m.udf);
  };

  if( wasm.exports.capdb_create_window_function ){
    __dbCleanupMap.addWindowFunc = function(pDb, name, arity){
      const m = __dbCleanupMap(pDb, 1);
      if(!m.wudf) m.wudf = new Map;
      this._addUDF(pDb, name, arity, m.wudf);
    };
  }

  /**
     Intended to be called _only_ from capdb_close_v2(),
     passed its non-0 db argument.

     This function frees up certain automatically-installed WASM
     function bindings which were installed on behalf of the given db,
     as those may otherwise leak.

     Notable caveat: this is only ever run via
     capdb.capi.capdb_close_v2(). If a client, for whatever
     reason, uses capdb.wasm.exports.capdb_close_v2() (the
     function directly exported from WASM), this cleanup will not
     happen.

     This is not a silver bullet for avoiding automation-related
     leaks but represents "an honest effort."

     The issue being addressed here is covered at:

     https://sqlite.org/wasm/doc/trunk/api-c-style.md#convert-func-ptr
  */
  __dbCleanupMap.cleanup = function(pDb){
    pDb = __argPDb(pDb);
    //wasm.xWrap.FuncPtrAdapter.debugFuncInstall = false;
    /**
       Installing NULL functions in the C API will remove those
       bindings. The FuncPtrAdapter which sits between us and the C
       API will also treat that as an opportunity to
       wasm.uninstallFunction() any WASM function bindings it has
       installed for pDb.
    */
    for(const obj of [
      /* pairs of [funcName, itsArityInC] */
      ['capdb_busy_handler',3],
      ['capdb_commit_hook',3],
      ['capdb_preupdate_hook',3],
      ['capdb_progress_handler',4],
      ['capdb_rollback_hook',3],
      ['capdb_set_authorizer',3],
      ['capdb_trace_v2', 4],
      ['capdb_update_hook',3]
      /*
        We do not yet have a way to clean up automatically-converted
        capdb_set_auxdata() finalizers.
      */
    ]){
      const [name, arity] = obj;
      const x = wasm.exports[name];
      if( !x ){
        /* assume it was built without this API */
        continue;
      }
      const closeArgs = [pDb];
      closeArgs.length = arity
      /* Recall that: (A) undefined entries translate to 0 when
         passed to WASM and (B) Safari wraps wasm.exports.* in
         nullary functions so x.length is 0 there. */;
      //wasm.xWrap.debug = true;
      try{ capi[name](...closeArgs) }
      catch(e){
        /* This "cannot happen" unless something is well and truly sideways. */
        capdb.config.warn("close-time call of",name+"(",closeArgs,") threw:",e);
      }
      //wasm.xWrap.debug = false;
    }
    const m = __dbCleanupMap(pDb, 0);
    if(!m) return;
    if(m.collation){
      for(const name of m.collation){
        try{
          capi.capdb_create_collation_v2(
            pDb, name, capi.CAPDB_UTF8, 0, 0, 0
          );
        }catch(e){
          /*ignored*/
        }
      }
      delete m.collation;
    }
    let i;
    for(i = 0; i < 2; ++i){ /* Clean up UDFs... */
      const fmap = i ? m.wudf : m.udf;
      if(!fmap) continue;
      const func = i
            ? capi.capdb_create_window_function
            : capi.capdb_create_function_v2;
      for(const e of fmap){
        const name = e[0], arities = e[1];
        const fargs = [pDb, name, 0/*arity*/, capi.CAPDB_UTF8, 0, 0, 0, 0, 0];
        if(i) fargs.push(0);
        for(const arity of arities){
          try{ fargs[2] = arity; func.apply(null, fargs); }
          catch(e){/*ignored*/}
        }
        arities.clear();
      }
      fmap.clear();
    }
    delete m.udf;
    delete m.wudf;
  }/*__dbCleanupMap.cleanup()*/;

  {/* Binding of capdb_close_v2() */
    const __capdbCloseV2 = wasm.xWrap("capdb_close_v2", "int", "capdb*");
    capi.capdb_close_v2 = function(pDb){
      if(1!==arguments.length) return __dbArgcMismatch(pDb, 'capdb_close_v2', 1);
      if(pDb){
        try{__dbCleanupMap.cleanup(pDb)} catch(e){/*ignored*/}
      }
      return __capdbCloseV2(pDb);
    };
  }/*capdb_close_v2()*/

  if(capi.capdb_session_create){
    const __capdbSessionDelete = wasm.xWrap(
      'capdb_session_delete', undefined, ['capdb_session*']
    );
    capi.capdb_session_delete = function(pSession){
      if(1!==arguments.length){
        return __dbArgcMismatch(pDb, 'capdb_session_delete', 1);
        /* Yes, we're returning a value from a void function. That seems
           like the lesser evil compared to not maintaining arg-count
           consistency as we do with other similar bindings. */
      }
      else if(pSession){
        //wasm.xWrap.FuncPtrAdapter.debugFuncInstall = true;
        capi.capdb_session_table_filter(pSession, 0, 0);
      }
      __capdbSessionDelete(pSession);
    };
  }

  {/* Bindings for capdb_create_collation[_v2]() */
    // contextKey() impl for wasm.xWrap.FuncPtrAdapter
    const contextKey = (argv,argIndex)=>{
      return 'argv['+argIndex+']:'+argv[0/* capdb* */]+
        ':'+wasm.cstrToJs(argv[1/* collation name */]).toLowerCase()
    };
    const __capdbCreateCollationV2 = wasm.xWrap(
      'capdb_create_collation_v2', 'int', [
        'capdb*', 'string', 'int', '*',
        new wasm.xWrap.FuncPtrAdapter({
          /* int(*xCompare)(void*,int,const void*,int,const void*) */
          name: 'xCompare', signature: 'i(pipip)', contextKey
        }),
        new wasm.xWrap.FuncPtrAdapter({
          /* void(*xDestroy(void*) */
          name: 'xDestroy', signature: 'v(p)', contextKey
        })
      ]
    );

    /**
       Works exactly like C's capdb_create_collation_v2() except that:

       1) It returns capi.CAPDB_FORMAT if the 3rd argument contains
          any encoding-related value other than capi.CAPDB_UTF8.  No
          other encodings are supported. As a special case, if the
          bottom 4 bits of that argument are 0, CAPDB_UTF8 is
          assumed.

       2) It accepts JS functions for its function-pointer arguments,
          for which it will install WASM-bound proxies. The bindings
          are "permanent," in that they will stay in the WASM
          environment until it shuts down unless the client calls this
          again with the same collation name and a value of 0 or null
          for the the function pointer(s). capdb_close_v2() will
          also clean up such automatically-installed WASM functions.

       For consistency with the C API, it requires the same number of
       arguments. It returns capi.CAPDB_MISUSE if passed any other
       argument count.

       Returns 0 on success, non-0 on error, in which case the error
       state of pDb (of type `capdb*` or argument-convertible to it)
       may contain more information.
    */
    capi.capdb_create_collation_v2 = function(pDb,zName,eTextRep,pArg,xCompare,xDestroy){
      if(6!==arguments.length) return __dbArgcMismatch(pDb, 'capdb_create_collation_v2', 6);
      else if( 0 === (eTextRep & 0xf) ){
        eTextRep |= capi.CAPDB_UTF8;
      }else if( capi.CAPDB_UTF8 !== (eTextRep & 0xf) ){
        return __errEncoding(pDb);
      }
      try{
        const rc = __capdbCreateCollationV2(pDb, zName, eTextRep, pArg, xCompare, xDestroy);
        if(0===rc && xCompare instanceof Function){
          __dbCleanupMap.addCollation(pDb, zName);
        }
        return rc;
      }catch(e){
        return util.capdb__wasm_db_error(pDb, e);
      }
    };

    capi.capdb_create_collation = (pDb,zName,eTextRep,pArg,xCompare)=>{
      return (5===arguments.length)
        ? capi.capdb_create_collation_v2(pDb,zName,eTextRep,pArg,xCompare,0)
        : __dbArgcMismatch(pDb, 'capdb_create_collation', 5);
    };

  }/*capdb_create_collation() and friends*/

  {/* Special-case handling of capdb_create_function_v2()
      and capdb_create_window_function(). */
    /** FuncPtrAdapter for contextKey() for capdb_create_function()
        and friends. */
    const contextKey = function(argv,argIndex){
      return (
        argv[0/* capdb* */]
          +':'+(argv[2/*number of UDF args*/] < 0 ? -1 : argv[2])
          +':'+argIndex/*distinct for each xAbc callback type*/
          +':'+wasm.cstrToJs(argv[1]).toLowerCase()
      )
    };

    /**
       JS proxies for the various capdb_create[_window]_function()
       callbacks, structured in a form usable by wasm.xWrap.FuncPtrAdapter.
    */
    const __cfProxy = Object.assign(Object.create(null), {
      xInverseAndStep: {
        signature:'v(pip)', contextKey,
        callProxy: (callback)=>{
          return (pCtx, argc, pArgv)=>{
            try{ callback(pCtx, ...capi.capdb_values_to_js(argc, pArgv)) }
            catch(e){ capi.capdb_result_error_js(pCtx, e) }
          };
        }
      },
      xFinalAndValue: {
        signature:'v(p)', contextKey,
        callProxy: (callback)=>{
          return (pCtx)=>{
            try{ capi.capdb_result_js(pCtx, callback(pCtx)) }
            catch(e){ capi.capdb_result_error_js(pCtx, e) }
          };
        }
      },
      xFunc: {
        signature:'v(pip)', contextKey,
        callProxy: (callback)=>{
          return (pCtx, argc, pArgv)=>{
            try{
              capi.capdb_result_js(
                pCtx,
                callback(pCtx, ...capi.capdb_values_to_js(argc, pArgv))
              );
            }catch(e){
              //console.error('xFunc() caught:',e);
              capi.capdb_result_error_js(pCtx, e);
            }
          };
        }
      },
      xDestroy: {
        signature:'v(p)', contextKey,
        //Arguable: a well-behaved destructor doesn't require a proxy.
        callProxy: (callback)=>{
          return (pVoid)=>{
            try{ callback(pVoid) }
            catch(e){ console.error("UDF xDestroy method threw:",e) }
          };
        }
      }
    })/*__cfProxy*/;

    const __capdbCreateFunction = wasm.xWrap(
      "capdb_create_function_v2", "int", [
        "capdb*", "string"/*funcName*/, "int"/*nArg*/,
        "int"/*eTextRep*/, "*"/*pApp*/,
        new wasm.xWrap.FuncPtrAdapter({name: 'xFunc', ...__cfProxy.xFunc}),
        new wasm.xWrap.FuncPtrAdapter({name: 'xStep', ...__cfProxy.xInverseAndStep}),
        new wasm.xWrap.FuncPtrAdapter({name: 'xFinal', ...__cfProxy.xFinalAndValue}),
        new wasm.xWrap.FuncPtrAdapter({name: 'xDestroy', ...__cfProxy.xDestroy})
      ]
    );

    const __capdbCreateWindowFunction =
          wasm.exports.capdb_create_window_function
          ? wasm.xWrap(
            "capdb_create_window_function", "int", [
              "capdb*", "string"/*funcName*/, "int"/*nArg*/,
              "int"/*eTextRep*/, "*"/*pApp*/,
              new wasm.xWrap.FuncPtrAdapter({name: 'xStep', ...__cfProxy.xInverseAndStep}),
              new wasm.xWrap.FuncPtrAdapter({name: 'xFinal', ...__cfProxy.xFinalAndValue}),
              new wasm.xWrap.FuncPtrAdapter({name: 'xValue', ...__cfProxy.xFinalAndValue}),
              new wasm.xWrap.FuncPtrAdapter({name: 'xInverse', ...__cfProxy.xInverseAndStep}),
              new wasm.xWrap.FuncPtrAdapter({name: 'xDestroy', ...__cfProxy.xDestroy})
            ]
          )
          : undefined;

    /* Documented in the api object's initializer. */
    capi.capdb_create_function_v2 = function f(
      pDb, funcName, nArg, eTextRep, pApp,
      xFunc,   //void (*xFunc)(capdb_context*,int,capdb_value**)
      xStep,   //void (*xStep)(capdb_context*,int,capdb_value**)
      xFinal,  //void (*xFinal)(capdb_context*)
      xDestroy //void (*xDestroy)(void*)
    ){
      if( f.length!==arguments.length ){
        return __dbArgcMismatch(pDb,"capdb_create_function_v2",f.length);
      }else if( 0 === (eTextRep & 0xf) ){
        eTextRep |= capi.CAPDB_UTF8;
      }else if( capi.CAPDB_UTF8 !== (eTextRep & 0xf) ){
        return __errEncoding(pDb);
      }
      try{
        const rc = __capdbCreateFunction(pDb, funcName, nArg, eTextRep,
                                           pApp, xFunc, xStep, xFinal, xDestroy);
        if(0===rc && (xFunc instanceof Function
                      || xStep instanceof Function
                      || xFinal instanceof Function
                      || xDestroy instanceof Function)){
          __dbCleanupMap.addFunction(pDb, funcName, nArg);
        }
        return rc;
      }catch(e){
        console.error("capdb_create_function_v2() setup threw:",e);
        return util.capdb__wasm_db_error(pDb, e, "Creation of UDF threw: "+e);
      }
    };

    /* Documented in the api object's initializer. */
    capi.capdb_create_function = function f(
      pDb, funcName, nArg, eTextRep, pApp,
      xFunc, xStep, xFinal
    ){
      return (f.length===arguments.length)
        ? capi.capdb_create_function_v2(pDb, funcName, nArg, eTextRep,
                                          pApp, xFunc, xStep, xFinal, 0)
        : __dbArgcMismatch(pDb,"capdb_create_function",f.length);
    };

    /* Documented in the api object's initializer. */
    if( __capdbCreateWindowFunction ){
      capi.capdb_create_window_function = function f(
        pDb, funcName, nArg, eTextRep, pApp,
        xStep,   //void (*xStep)(capdb_context*,int,capdb_value**)
        xFinal,  //void (*xFinal)(capdb_context*)
        xValue,  //void (*xValue)(capdb_context*)
        xInverse,//void (*xInverse)(capdb_context*,int,capdb_value**)
        xDestroy //void (*xDestroy)(void*)
      ){
        if( f.length!==arguments.length ){
          return __dbArgcMismatch(pDb,"capdb_create_window_function",f.length);
        }else if( 0 === (eTextRep & 0xf) ){
          eTextRep |= capi.CAPDB_UTF8;
        }else if( capi.CAPDB_UTF8 !== (eTextRep & 0xf) ){
          return __errEncoding(pDb);
        }
        try{
          const rc = __capdbCreateWindowFunction(pDb, funcName, nArg, eTextRep,
                                                   pApp, xStep, xFinal, xValue,
                                                   xInverse, xDestroy);
          if(0===rc && (xStep instanceof Function
                        || xFinal instanceof Function
                        || xValue instanceof Function
                        || xInverse instanceof Function
                        || xDestroy instanceof Function)){
            __dbCleanupMap.addWindowFunc(pDb, funcName, nArg);
          }
          return rc;
        }catch(e){
          console.error("capdb_create_window_function() setup threw:",e);
          return util.capdb__wasm_db_error(pDb, e, "Creation of UDF threw: "+e);
        }
      };
    }else{
      delete capi.capdb_create_window_function;
    }
    /**
       A _deprecated_ alias for capi.capdb_result_js() which
       predates the addition of that function in the public API.
    */
    capi.capdb_create_function_v2.udfSetResult =
      capi.capdb_create_function.udfSetResult = capi.capdb_result_js;
    if(capi.capdb_create_window_function){
      capi.capdb_create_window_function.udfSetResult = capi.capdb_result_js;
    }

    /**
       A _deprecated_ alias for capi.capdb_values_to_js() which
       predates the addition of that function in the public API.
    */
    capi.capdb_create_function_v2.udfConvertArgs =
      capi.capdb_create_function.udfConvertArgs = capi.capdb_values_to_js;
    if(capi.capdb_create_window_function){
      capi.capdb_create_window_function.udfConvertArgs = capi.capdb_values_to_js;
    }

    /**
       A _deprecated_ alias for capi.capdb_result_error_js() which
       predates the addition of that function in the public API.
    */
    capi.capdb_create_function_v2.udfSetError =
      capi.capdb_create_function.udfSetError = capi.capdb_result_error_js;
    if(capi.capdb_create_window_function){
      capi.capdb_create_window_function.udfSetError = capi.capdb_result_error_js;
    }

  }/*capdb_create_function_v2() and capdb_create_window_function() proxies*/;

  {/* Special-case handling of capdb_prepare_v2() and
      capdb_prepare_v3() */

    /**
       Helper for string:flexible conversions which requires a
       byte-length counterpart argument. Passed a value and its
       ostensible length, this function returns [V,N], where V is
       either v or a to-string transformed copy of v and N is either n
       (if v is a WASM pointer, in which case n might be a BigInt), -1
       (if v is a string or Array), or the byte length of v (if it's a
       byte array or ArrayBuffer).
    */
    const __flexiString = (v,n)=>{
      if('string'===typeof v){
        n = -1;
      }else if(util.isSQLableTypedArray(v)){
        n = v.byteLength;
        v = wasm.typedArrayToString(
          (v instanceof ArrayBuffer) ? new Uint8Array(v) : v
        );
      }else if(Array.isArray(v)){
        v = v.join("");
        n = -1;
      }
      return [v, n];
    };

    /**
       Scope-local holder of the two impls of capdb_prepare_v2/v3().
    */
    const __prepare = {
      /**
         This binding expects a JS string as its 2nd argument and
         null as its final argument. In order to compile multiple
         statements from a single string, the "full" impl (see
         below) must be used.
      */
      basic: wasm.xWrap('capdb_prepare_v3',
                        "int", ["capdb*", "string",
                                "int"/*ignored for this impl!*/,
                                "int", "**",
                                "**"/*MUST be 0 or null or undefined!*/]),
      /**
         Impl which requires that the 2nd argument be a pointer to the
         SQL string, instead of being converted to a JS string. This
         variant is necessary for cases where we require a non-NULL
         value for the final argument (prepare/step of multiple
         statements from one input string). For simpler cases, where
         only the first statement in the SQL string is required, the
         wrapper named capdb_prepare_v2() is sufficient and easier
         to use because it doesn't require dealing with pointers.
      */
      full: wasm.xWrap('capdb_prepare_v3',
                       "int", ["capdb*", "*", "int", "int",
                               "**", "**"])
    };

    /* Documented in the capi object's initializer. */
    capi.capdb_prepare_v3 = function f(pDb, sql, sqlLen, prepFlags, ppStmt, pzTail){
      if(f.length!==arguments.length){
        return __dbArgcMismatch(pDb,"capdb_prepare_v3",f.length);
      }
      const [xSql, xSqlLen] = __flexiString(sql, Number(sqlLen));
      switch(typeof xSql){
        case 'string': return __prepare.basic(pDb, xSql, xSqlLen, prepFlags, ppStmt, null);
        case (typeof wasm.ptr.null):
          return __prepare.full(pDb, wasm.ptr.coerce(xSql), xSqlLen, prepFlags,
                                ppStmt, pzTail);
        default:
          return util.capdb__wasm_db_error(
            pDb, capi.CAPDB_MISUSE,
            "Invalid SQL argument type for capdb_prepare_v2/v3(). typeof="+(typeof xSql)
          );
      }
    };

    /* Documented in the capi object's initializer. */
    capi.capdb_prepare_v2 = function f(pDb, sql, sqlLen, ppStmt, pzTail){
      return (f.length===arguments.length)
        ? capi.capdb_prepare_v3(pDb, sql, sqlLen, 0, ppStmt, pzTail)
        : __dbArgcMismatch(pDb,"capdb_prepare_v2",f.length);
    };

  }/*capdb_prepare_v2/v3()*/

  {/*capdb_bind_text/blob()*/
    const __bindText = wasm.xWrap("capdb_bind_text", "int", [
      "capdb_stmt*", "int", "string", "int", "*"
    ]);
    const __bindBlob = wasm.xWrap("capdb_bind_blob", "int", [
      "capdb_stmt*", "int", "*", "int", "*"
    ]);

    /** Documented in the capi object's initializer. */
    capi.capdb_bind_text = function f(pStmt, iCol, text, nText, xDestroy){
      if(f.length!==arguments.length){
        return __dbArgcMismatch(capi.capdb_db_handle(pStmt),
                                "capdb_bind_text", f.length);
      }else if(wasm.isPtr(text) || null===text){
        return __bindText(pStmt, iCol, text, nText, xDestroy);
      }else if(text instanceof ArrayBuffer){
        text = new Uint8Array(text);
      }else if(Array.isArray(pMem)){
        text = pMem.join('');
      }
      let p, n;
      try{
        if(util.isSQLableTypedArray(text)){
          p = wasm.allocFromTypedArray(text);
          n = text.byteLength;
        }else if('string'===typeof text){
          [p, n] = wasm.allocCString(text);
        }else{
          return util.capdb__wasm_db_error(
            capi.capdb_db_handle(pStmt), capi.CAPDB_MISUSE,
            "Invalid 3rd argument type for capdb_bind_text()."
          );
        }
        return __bindText(pStmt, iCol, p, n, capi.CAPDB_WASM_DEALLOC);
      }catch(e){
        wasm.dealloc(p);
        return util.capdb__wasm_db_error(
          capi.capdb_db_handle(pStmt), e
        );
      }
    }/*capdb_bind_text()*/;

    /** Documented in the capi object's initializer. */
    capi.capdb_bind_blob = function f(pStmt, iCol, pMem, nMem, xDestroy){
      if(f.length!==arguments.length){
        return __dbArgcMismatch(capi.capdb_db_handle(pStmt),
                                "capdb_bind_blob", f.length);
      }else if(wasm.isPtr(pMem) || null===pMem){
        return __bindBlob(pStmt, iCol, pMem, nMem, xDestroy);
      }else if(pMem instanceof ArrayBuffer){
        pMem = new Uint8Array(pMem);
      }else if(Array.isArray(pMem)){
        pMem = pMem.join('');
      }
      let p, n;
      try{
        if(util.isBindableTypedArray(pMem)){
          p = wasm.allocFromTypedArray(pMem);
          n = nMem>=0 ? nMem : pMem.byteLength;
        }else if('string'===typeof pMem){
          [p, n] = wasm.allocCString(pMem);
        }else{
          return util.capdb__wasm_db_error(
            capi.capdb_db_handle(pStmt), capi.CAPDB_MISUSE,
            "Invalid 3rd argument type for capdb_bind_blob()."
          );
        }
        return __bindBlob(pStmt, iCol, p, n, capi.CAPDB_WASM_DEALLOC);
      }catch(e){
        wasm.dealloc(p);
        return util.capdb__wasm_db_error(
          capi.capdb_db_handle(pStmt), e
        );
      }
    }/*capdb_bind_blob()*/;

  }/*capdb_bind_text/blob()*/

//#if proxy-text-apis
  if(!capi.capdb_column_text){
    /*[tag:proxy-text-apis]
      As discussed at:

      https://sqlite.org/forum/forumpost/d77281aec2df9ada

      Summary: there are opinions that capdb_column_text() and
      capdb_value_text() should handle strings such that embedded
      NULs are retained. This block does that. This block does _not_
      apply that special-case behavior to any number of _other_
      APIs which return C-strings. That discrepancy makes this
      block highly arguable, but one can also argue that these two
      specific functions can get away with such acrobatics without
      it being called voodoo in a pejorative sense.
    */
    const argStmt  = wasm.xWrap.argAdapter('capdb_stmt*'),
          argInt   = wasm.xWrap.argAdapter('int'),
          argValue = wasm.xWrap.argAdapter('capdb_value*'),
          newStr   =
          (cstr,n)=>wasm.typedArrayToString(wasm.heap8u(),
                                           Number(cstr), Number(cstr)+n)
    capi.capdb_column_text = function(stmt, colIndex){
      const a0 = argStmt(stmt), a1 = argInt(colIndex);
      const cstr = wasm.exports.capdb_column_text(a0, a1);
      return cstr
        ? newStr(cstr,wasm.exports.capdb_column_bytes(a0, a1))
        : null;
    };
    capi.capdb_value_text = function(val){
      const a0 = argValue(val);
      const cstr = wasm.exports.capdb_value_text(a0);
      return cstr
        ? newStr(cstr,wasm.exports.capdb_value_bytes(a0))
        : null;
    };
  }/*text-return-related bindings*/
//#/if proxy-text-apis

  {/* capdb_config() */
    /**
       Wraps a small subset of the C API's capdb_config() options.
       Unsupported options trigger the return of capi.CAPDB_NOTFOUND.
       Passing fewer than 2 arguments triggers return of
       capi.CAPDB_MISUSE.
    */
    capi.capdb_config = function(op, ...args){
      if(arguments.length<2) return capi.CAPDB_MISUSE;
      switch(op){
          case capi.CAPDB_CONFIG_COVERING_INDEX_SCAN: // 20  /* int */
          case capi.CAPDB_CONFIG_MEMSTATUS:// 9  /* boolean */
          case capi.CAPDB_CONFIG_SMALL_MALLOC: // 27  /* boolean */
          case capi.CAPDB_CONFIG_SORTERREF_SIZE: // 28  /* int nByte */
          case capi.CAPDB_CONFIG_STMTJRNL_SPILL: // 26  /* int nByte */
          case capi.CAPDB_CONFIG_URI:// 17  /* int */
            return wasm.exports.capdb__wasm_config_i(op, args[0]);
          case capi.CAPDB_CONFIG_LOOKASIDE: // 13  /* int int */
            return wasm.exports.capdb__wasm_config_ii(op, args[0], args[1]);
          case capi.CAPDB_CONFIG_MEMDB_MAXSIZE: // 29  /* capdb_int64 */
            return wasm.exports.capdb__wasm_config_j(op, args[0]);
          case capi.CAPDB_CONFIG_GETMALLOC: // 5 /* capdb_mem_methods* */
          case capi.CAPDB_CONFIG_GETMUTEX: // 11  /* capdb_mutex_methods* */
          case capi.CAPDB_CONFIG_GETPCACHE2: // 19  /* capdb_pcache_methods2* */
          case capi.CAPDB_CONFIG_GETPCACHE: // 15  /* no-op */
          case capi.CAPDB_CONFIG_HEAP: // 8  /* void*, int nByte, int min */
          case capi.CAPDB_CONFIG_LOG: // 16  /* xFunc, void* */
          case capi.CAPDB_CONFIG_MALLOC:// 4  /* capdb_mem_methods* */
          case capi.CAPDB_CONFIG_MMAP_SIZE: // 22  /* capdb_int64, capdb_int64 */
          case capi.CAPDB_CONFIG_MULTITHREAD: // 2 /* nil */
          case capi.CAPDB_CONFIG_MUTEX: // 10  /* capdb_mutex_methods* */
          case capi.CAPDB_CONFIG_PAGECACHE: // 7  /* void*, int sz, int N */
          case capi.CAPDB_CONFIG_PCACHE2: // 18  /* capdb_pcache_methods2* */
          case capi.CAPDB_CONFIG_PCACHE: // 14  /* no-op */
          case capi.CAPDB_CONFIG_PCACHE_HDRSZ: // 24  /* int *psz */
          case capi.CAPDB_CONFIG_PMASZ: // 25  /* unsigned int szPma */
          case capi.CAPDB_CONFIG_SERIALIZED: // 3 /* nil */
          case capi.CAPDB_CONFIG_SINGLETHREAD: // 1 /* nil */:
          case capi.CAPDB_CONFIG_SQLLOG: // 21  /* xSqllog, void* */
          case capi.CAPDB_CONFIG_WIN32_HEAPSIZE: // 23  /* int nByte */
          default:
          /* maintenance note: we specifically do not include
             CAPDB_CONFIG_ROWID_IN_VIEW here, on the grounds that
             it's only for legacy support and no apps written with
             this API require that. */
            return capi.CAPDB_NOTFOUND;
      }
    };
  }/* capdb_config() */

  {/*auto-extension bindings.*/
    const __autoExtFptr = new Set;

    capi.capdb_auto_extension = function(fPtr){
      if( fPtr instanceof Function ){
        fPtr = wasm.installFunction('i(ppp)', fPtr);
      }else if( 1!==arguments.length || !wasm.isPtr(fPtr) ){
        return capi.CAPDB_MISUSE;
      }
      const rc = wasm.exports.capdb_auto_extension(fPtr);
      if( fPtr!==arguments[0] ){
        if(0===rc) __autoExtFptr.add(fPtr);
        else wasm.uninstallFunction(fPtr);
      }
      return rc;
    };

    capi.capdb_cancel_auto_extension = function(fPtr){
     /* We do not do an automatic JS-to-WASM function conversion here
        because it would be senseless: the converted pointer would
        never possibly match an already-installed one. */;
      if(!fPtr || 1!==arguments.length || !wasm.isPtr(fPtr)) return 0;
      return wasm.exports.capdb_cancel_auto_extension(fPtr);
      /* Note that it "cannot happen" that a client passes a pointer which
         is in __autoExtFptr because __autoExtFptr only contains automatic
         conversions created inside capdb_auto_extension() and
         never exposed to the client. */
    };

    capi.capdb_reset_auto_extension = function(){
      wasm.exports.capdb_reset_auto_extension();
      for(const fp of __autoExtFptr) wasm.uninstallFunction(fp);
      __autoExtFptr.clear();
    };
  }/* auto-extension */

  /* Warn if client-level code makes use of FuncPtrAdapter. */
  wasm.xWrap.FuncPtrAdapter.warnOnUse = true;

  const StructBinder = capdb.StructBinder
  /* we require a local alias b/c StructBinder is removed from the capdb
     object during the final steps of the API cleanup. */;
  /**
     Installs a StructBinder-bound function pointer member of the
     given name and function in the given StructBinder.StructType
     target object.

     It creates a WASM proxy for the given function and arranges for
     that proxy to be cleaned up when tgt.dispose() is called. Throws
     on the slightest hint of error, e.g. tgt is-not-a StructType,
     name does not map to a struct-bound member, etc.

     As a special case, if the given function is a pointer, then
     `wasm.functionEntry()` is used to validate that it is a known
     function. If so, it is used as-is with no extra level of proxying
     or cleanup, else an exception is thrown. It is legal to pass a
     value of 0, indicating a NULL pointer, with the caveat that 0
     _is_ a legal function pointer in WASM but it will not be accepted
     as such _here_. (Justification: the function at address zero must
     be one which initially came from the WASM module, not a method we
     want to bind to a virtual table or VFS.)

     This function returns a proxy for itself which is bound to tgt
     and takes 2 args (name,func). That function returns the same
     thing as this one, permitting calls to be chained.

     If called with only 1 arg, it has no side effects but returns a
     func with the same signature as described above.

     ACHTUNG: because we cannot generically know how to transform JS
     exceptions into result codes, the installed functions do no
     automatic catching of exceptions. It is critical, to avoid
     undefined behavior in the C layer, that methods mapped via
     this function do not throw. The exception, as it were, to that
     rule is...

     If applyArgcCheck is true then each JS function (as opposed to
     function pointers) gets wrapped in a proxy which asserts that it
     is passed the expected number of arguments, throwing if the
     argument count does not match expectations. That is only intended
     for dev-time usage for sanity checking, and may leave the C
     environment in an undefined state.
  */
  const installMethod = function callee(
    tgt, name, func, applyArgcCheck = callee.installMethodArgcCheck
  ){
    if(!(tgt instanceof StructBinder.StructType)){
      toss("Usage error: target object is-not-a StructType.");
    }else if(!(func instanceof Function) && !wasm.isPtr(func)){
      toss("Usage error: expecting a Function or WASM pointer to one.");
    }
    if(1===arguments.length){
      return (n,f)=>callee(tgt, n, f, applyArgcCheck);
    }
    if(!callee.argcProxy){
      callee.argcProxy = function(tgt, funcName, func,sig){
        return function(...args){
          if(func.length!==arguments.length){
            toss("Argument mismatch for",
                 tgt.structInfo.name+"::"+funcName
                 +": Native signature is:",sig);
          }
          return func.apply(this, args);
        }
      };
      /* An ondispose() callback for use with
         StructBinder-created types. */
      callee.removeFuncList = function(){
        if(this.ondispose.__removeFuncList){
          this.ondispose.__removeFuncList.forEach(
            (v,ndx)=>{
              if(wasm.isPtr(v)){
                try{wasm.uninstallFunction(v)}
                catch(e){/*ignore*/}
              }
              /* else it's a descriptive label for the next number in
                 the list. */
            }
          );
          delete this.ondispose.__removeFuncList;
        }
      };
    }/*static init*/
    const sigN = tgt.memberSignature(name);
    if(sigN.length<2){
      toss("Member",name,"does not have a function pointer signature:",sigN);
    }
    const memKey = tgt.memberKey(name);
    const fProxy = (applyArgcCheck && !wasm.isPtr(func))
    /** This middle-man proxy is only for use during development, to
        confirm that we always pass the proper number of
        arguments. We know that the C-level code will always use the
        correct argument count. */
          ? callee.argcProxy(tgt, memKey, func, sigN)
          : func;
    if(wasm.isPtr(fProxy)){
      if(fProxy && !wasm.functionEntry(fProxy)){
        toss("Pointer",fProxy,"is not a WASM function table entry.");
      }
      tgt[memKey] = fProxy;
    }else{
      const pFunc = wasm.installFunction(fProxy, sigN);
      tgt[memKey] = pFunc;
      if(!tgt.ondispose || !tgt.ondispose.__removeFuncList){
        tgt.addOnDispose('ondispose.__removeFuncList handler',
                         callee.removeFuncList);
        tgt.ondispose.__removeFuncList = [];
      }
      tgt.ondispose.__removeFuncList.push(memKey, pFunc);
    }
    return (n,f)=>callee(tgt, n, f, applyArgcCheck);
  }/*installMethod*/;
  installMethod.installMethodArgcCheck = false;

  /**
     Installs methods into the given StructBinder.StructType-type
     instance. Each entry in the given methods object must map to a
     known member of the given StructType, else an exception will be
     triggered.  See installMethod() for more details, including the
     semantics of the 3rd argument.

     As an exception to the above, if any two or more methods in the
     2nd argument are the exact same function, installMethod() is
     _not_ called for the 2nd and subsequent instances, and instead
     those instances get assigned the same method pointer which is
     created for the first instance. This optimization is primarily to
     accommodate special handling of capdb_module::xConnect and
     xCreate methods.

     On success, returns its first argument. Throws on error.
  */
  const installMethods = function(
    structInstance, methods, applyArgcCheck = installMethod.installMethodArgcCheck
  ){
    const seen = new Map /* map of <Function, memberName> */;
    for(const k of Object.keys(methods)){
      const m = methods[k];
      const prior = seen.get(m);
      if(prior){
        const mkey = structInstance.memberKey(k);
        structInstance[mkey] = structInstance[structInstance.memberKey(prior)];
      }else{
        installMethod(structInstance, k, m, applyArgcCheck);
        seen.set(m, k);
      }
    }
    return structInstance;
  };

  /**
     Equivalent to calling installMethod(this,...arguments) with a
     first argument of this object. If called with 1 or 2 arguments
     and the first is an object, it's instead equivalent to calling
     installMethods(this,...arguments).
  */
  StructBinder.StructType.prototype.installMethod = function callee(
    name, func, applyArgcCheck = installMethod.installMethodArgcCheck
  ){
    return (arguments.length < 3 && name && 'object'===typeof name)
      ? installMethods(this, ...arguments)
      : installMethod(this, ...arguments);
  };

  /**
     Equivalent to calling installMethods() with a first argument
     of this object.
  */
  StructBinder.StructType.prototype.installMethods = function(
    methods, applyArgcCheck = installMethod.installMethodArgcCheck
  ){
    return installMethods(this, methods, applyArgcCheck);
  };

});
