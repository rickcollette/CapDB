//#if not target:node
/*
  2022-09-18

  The author disclaims copyright to this source code.  In place of a
  legal notice, here is a blessing:

  *   May you do good and not evil.
  *   May you find forgiveness for yourself and forgive others.
  *   May you share freely, never taking more than you give.

  ***********************************************************************

  This file holds the synchronous half of an capdb_vfs
  implementation which proxies, in a synchronous fashion, the
  asynchronous Origin-Private FileSystem (OPFS) APIs using a second
  Worker, implemented in capdb-opfs-async-proxy.js.  This file is
  intended to be appended to the main capdb JS deliverable somewhere
  after capdb-api-oo1.js.
*/
'use strict';
globalThis.capdbApiBootstrap.initializers.push(function(capdb){
  if( !capdb.opfs || capdb.config.disable?.vfs?.opfs ){
    return;
  }
  const util = capdb.util,
        opfsUtil = capdb.opfs || capdb.util.toss("Missing capdb.opfs");
  /**
   installOpfsVfs() returns a Promise which, on success, installs an
   capdb_vfs named "opfs", suitable for use with all capdb APIs
   which accept a VFS. It is intended to be called via
   capdbApiBootstrap.initializers or an equivalent mechanism.

   The installed VFS uses the Origin-Private FileSystem API for
   all file storage. On error it is rejected with an exception
   explaining the problem. Reasons for rejection include, but are
   not limited to:

   - The counterpart Worker (see below) could not be loaded.

   - The environment does not support OPFS. That includes when
     this function is called from the main window thread.

  Significant notes and limitations:

  - The OPFS features used here are only available in dedicated Worker
    threads. This file tries to detect that case, resulting in a
    rejected Promise if those features do not seem to be available.

  - It requires the SharedArrayBuffer and Atomics classes, and the
    former is only available if the HTTP server emits the so-called
    COOP and COEP response headers. These features are required for
    proxying OPFS's synchronous API via the synchronous interface
    required by the capdb_vfs API.

  - This function may only be called a single time. When called, this
    function removes itself from the capdb object.

  All arguments to this function are for internal/development purposes
  only. They do not constitute a public API and may change at any
  time.

  The argument may optionally be a plain object with the following
  configuration options:

  - proxyUri: name of the async proxy JS file or a synchronous function
    which, when called, returns such a name.

  - verbose (=2): an integer 0-3. 0 disables all logging, 1 enables
    logging of errors. 2 enables logging of warnings and errors. 3
    additionally enables debugging info. Logging is performed
    via the capdb.config.{log|warn|error}() functions.

  - sanityChecks (=false): if true, some basic sanity tests are run on
    the OPFS VFS API after it's initialized, before the returned
    Promise resolves. This is only intended for testing and
    development of the VFS, not client-side use.

  Additionaly, the (officially undocumented) 'opfs-disable' URL
  argument will disable OPFS, making this function a no-op.

  On success, the Promise resolves to the top-most capdb namespace
  object. Success does not necessarily mean that it installs the VFS,
  as there are legitimate non-error reasons for OPFS not to be
  available.
*/
const installOpfsVfs = async function(options){
  options = opfsUtil.initOptions('opfs',options);
  if( !options ) return capdb;
  const capi = capdb.capi,
        state = opfsUtil.createVfsState(),
        opfsVfs = state.vfs,
        metrics = opfsVfs.metrics.counters,
        mTimeStart = opfsVfs.mTimeStart,
        mTimeEnd = opfsVfs.mTimeEnd,
        opRun = opfsVfs.opRun,
        debug = (...args)=>capdb.config.debug("opfs:",...args),
        warn = (...args)=>capdb.config.warn("opfs:",...args),
        __openFiles = opfsVfs.__openFiles;

  //debug("options:",JSON.stringify(options));
  /*
    At this point, createVfsState() has populated:

    - state: the configuration object we share with the async proxy.

    - opfsVfs: an capdb_vfs instance with lots of JS state attached
    to it.

    with any code common to both the "opfs" and "opfs-wl" VFSes. Now
    comes the VFS-dependent work...
  */
  return opfsVfs.bindVfs(util.nu({
    xLock: function(pFile,lockType){
      mTimeStart('xLock');
      ++metrics.xLock.count;
      const f = __openFiles[pFile];
      let rc = 0;
      /* All OPFS locks are exclusive locks. If xLock() has
         previously succeeded, do nothing except record the lock
         type. If no lock is active, have the async counterpart
         lock the file. */
      if( f.lockType ) {
        f.lockType = lockType;
      }else{
        rc = opRun('xLock', pFile, lockType);
        if( 0===rc ) f.lockType = lockType;
      }
      mTimeEnd();
      return rc;
    },
    xUnlock: function(pFile,lockType){
      mTimeStart('xUnlock');
      ++metrics.xUnlock.count;
      const f = __openFiles[pFile];
      let rc = 0;
      if( capi.CAPDB_LOCK_NONE === lockType
          && f.lockType ){
        rc = opRun('xUnlock', pFile, lockType);
      }
      if( 0===rc ) f.lockType = lockType;
      mTimeEnd();
      return rc;
    }
  }), function(capdb, vfs){
    /* Post-VFS-registration initialization... */
    if(capdb.oo1){
      const OpfsDb = function(...args){
        const opt = capdb.oo1.DB.dbCtorHelper.normalizeArgs(...args);
        opt.vfs = vfs.$zName;
        capdb.oo1.DB.dbCtorHelper.call(this, opt);
      };
      OpfsDb.prototype = Object.create(capdb.oo1.DB.prototype);
      capdb.oo1.OpfsDb = OpfsDb;
      OpfsDb.importDb = opfsUtil.importDb;
      if( true ){
        /* 2026-03-06: this was a design mis-decision and is
           inconsistent with capdb_open() and friends, but is
           retained against the risk of introducing regressions if
           it's removed. */
        capdb.oo1.DB.dbCtorHelper.setVfsPostOpenCallback(
          opfsVfs.pointer,
          function(oo1Db, capdb){
            /* Set a relatively high default busy-timeout handler to
               help OPFS dbs deal with multi-tab/multi-worker
               contention. */
            capdb.capi.capdb_busy_timeout(oo1Db, 10000);
          }
        );
      }
    }/*extend capdb.oo1*/
  })/*bindVfs()*/;
}/*installOpfsVfs()*/;
globalThis.capdbApiBootstrap.initializersAsync.push(async (capdb)=>{
  return installOpfsVfs().catch((e)=>{
    capdb.config.warn("Ignoring inability to install 'opfs' capdb_vfs:",e);
  })
});
}/*capdbApiBootstrap.initializers.push()*/);
//#/if target:node
