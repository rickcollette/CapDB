//#if not target:node
/*
  2026-02-20

  The author disclaims copyright to this source code.  In place of a
  legal notice, here is a blessing:

  *   May you do good and not evil.
  *   May you find forgiveness for yourself and forgive others.
  *   May you share freely, never taking more than you give.

  ***********************************************************************

  This file is a reimplementation of the "opfs" VFS (as distinct from
  "opfs-sahpool") which uses WebLocks for locking instead of a bespoke
  Atomics.wait()/notify() protocol. This file holds the "synchronous
  half" of the VFS, whereas it shares the "asynchronous half" with the
  "opfs" VFS.

  Testing has failed to show any genuine functional difference between
  these VFSes other than "opfs-wl" being able to dole out xLock()
  requests in a strictly FIFO manner by virtue of WebLocks being
  globally managed by the browser. This tends to lead to, but does not
  guaranty, fairer distribution of locks. Differences are unlikely to
  be noticed except, perhaps, under very high contention.

  This file is intended to be appended to the main capdb JS
  deliverable somewhere after opfs-common-shared.c-pp.js.
*/
'use strict';
globalThis.capdbApiBootstrap.initializers.push(function(capdb){
  if( !capdb.opfs || capdb.config.disable?.vfs?.['opfs-wl'] ){
    return;
  }
  const util = capdb.util,
        toss  = capdb.util.toss;
  const opfsUtil = capdb.opfs;
  const vfsName = 'opfs-wl';
/**
   installOpfsWlVfs() returns a Promise which, on success, installs an
   capdb_vfs named "opfs-wl", suitable for use with all capdb APIs
   which accept a VFS. It is intended to be called via
   capdbApiBootstrap.initializers or an equivalent mechanism.

   This VFS is essentially identical to the "opfs" VFS but uses
   WebLocks for its xLock() and xUnlock() implementations.

   Quirks specific to this VFS:

   - The (officially undocumented) 'opfs-wl-disable' URL
   argument will disable OPFS, making this function a no-op.

   Aside from locking differences in the VFSes, this function
   otherwise behaves the same as
   capdb-vfs-opfs.c-pp.js:installOpfsVfs().
*/
const installOpfsWlVfs = async function(options){
  options = opfsUtil.initOptions(vfsName,options);
  if( !options ) return capdb;
  const capi = capdb.capi,
        state = opfsUtil.createVfsState(),
        opfsVfs = state.vfs,
        metrics = opfsVfs.metrics.counters,
        mTimeStart = opfsVfs.mTimeStart,
        mTimeEnd = opfsVfs.mTimeEnd,
        opRun = opfsVfs.opRun,
        debug = (...args)=>capdb.config.debug(vfsName+":",...args),
        warn = (...args)=>capdb.config.warn(vfsName+":",...args),
        __openFiles = opfsVfs.__openFiles;

  //debug("state",JSON.stringify(options));
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
      //debug("xLock()...");
      const f = __openFiles[pFile];
      const rc = opRun('xLock', pFile, lockType);
      if( !rc ) f.lockType = lockType;
      mTimeEnd();
      return rc;
    },
    xUnlock: function(pFile,lockType){
      mTimeStart('xUnlock');
      const f = __openFiles[pFile];
      const rc = opRun('xUnlock', pFile, lockType);
      if( !rc ) f.lockType = lockType;
      mTimeEnd();
      return rc;
    }
  }), function(capdb, vfs){
    /* Post-VFS-registration initialization... */
    if(capdb.oo1){
      const OpfsWlDb = function(...args){
        const opt = capdb.oo1.DB.dbCtorHelper.normalizeArgs(...args);
        opt.vfs = vfs.$zName;
        capdb.oo1.DB.dbCtorHelper.call(this, opt);
      };
      OpfsWlDb.prototype = Object.create(capdb.oo1.DB.prototype);
      capdb.oo1.OpfsWlDb = OpfsWlDb;
      OpfsWlDb.importDb = opfsUtil.importDb;
      /* The "opfs" VFS variant adds a
         oo1.DB.dbCtorHelper.setVfsPostOpenCallback() callback to set
         a high busy_timeout. That was a design mis-decision and is
         inconsistent with capdb_open() and friends, but is retained
         against the risk of introducing regressions if it's removed.
         This variant does not repeat that mistake.
      */
    }
  })/*bindVfs()*/;
}/*installOpfsWlVfs()*/;
globalThis.capdbApiBootstrap.initializersAsync.push(async (capdb)=>{
  return installOpfsWlVfs().catch((e)=>{
    capdb.config.warn("Ignoring inability to install the",vfsName,"capdb_vfs:",e);
  });
});
}/*capdbApiBootstrap.initializers.push()*/);
//#/if target:node
