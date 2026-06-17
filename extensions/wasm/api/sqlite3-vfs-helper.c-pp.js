/*
** 2022-11-30
**
** The author disclaims copyright to this source code.  In place of a
** legal notice, here is a blessing:
**
** *   May you do good and not evil.
** *   May you find forgiveness for yourself and forgive others.
** *   May you share freely, never taking more than you give.
*/

/**
   This file installs capdb.vfs, a namespace of helpers for use in
   the creation of JavaScript implementations of capdb_vfs.
*/
'use strict';
globalThis.capdbApiBootstrap.initializers.push(function(capdb){
  const wasm = capdb.wasm, capi = capdb.capi, toss = capdb.util.toss3;
  const vfs = Object.create(null);
  capdb.vfs = vfs;

  /**
     Uses capdb_vfs_register() to register this
     capdb.capi.capdb_vfs instance. This object must have already
     been filled out properly. If the first argument is truthy, the
     VFS is registered as the default VFS, else it is not.

     On success, returns this object. Throws on error.
  */
  capi.capdb_vfs.prototype.registerVfs = function(asDefault=false){
    if(!(this instanceof capdb.capi.capdb_vfs)){
      toss("Expecting a capdb_vfs-type argument.");
    }
    const rc = capi.capdb_vfs_register(this, asDefault ? 1 : 0);
    if(rc){
      toss("capdb_vfs_register(",this,") failed with rc",rc);
    }
    if(this.pointer !== capi.capdb_vfs_find(this.$zName)){
      toss("BUG: capdb_vfs_find(vfs.$zName) failed for just-installed VFS",
           this);
    }
    return this;
  };

  /**
     A wrapper for
     capdb.StructBinder.StructType.prototype.installMethods() or
     registerVfs() to reduce installation of a VFS and/or its I/O
     methods to a single call.

     Accepts an object which contains the properties "io" and/or
     "vfs", each of which is itself an object with following properties:

     - `struct`: an capdb.StructBinder.StructType-type struct. This
       must be a populated (except for the methods) object of type
       capdb_io_methods (for the "io" entry) or capdb_vfs (for the
       "vfs" entry).

     - `methods`: an object mapping capdb_io_methods method names
       (e.g. 'xClose') to JS implementations of those methods. The JS
       implementations must be call-compatible with their native
       counterparts.

     For each of those object, this function passes its (`struct`,
     `methods`, (optional) `applyArgcCheck`) properties to
     installMethods().

     If the `vfs` entry is set then:

     - Its `struct` property's registerVfs() is called. The
       `vfs` entry may optionally have an `asDefault` property, which
       gets passed as the argument to registerVfs().

     - If `struct.$zName` is falsy and the entry has a string-type
       `name` property, `struct.$zName` is set to the C-string form of
       that `name` value before registerVfs() is called. That string
       gets added to the on-dispose state of the struct.

     On success returns this object. Throws on error.
  */
  vfs.installVfs = function(opt){
    let count = 0;
    const propList = ['io','vfs'];
    for(const key of propList){
      const o = opt[key];
      if(o){
        ++count;
        o.struct.installMethods(o.methods, !!o.applyArgcCheck);
        if('vfs'===key){
          if(!o.struct.$zName && 'string'===typeof o.name){
            o.struct.addOnDispose(
              o.struct.$zName = wasm.allocCString(o.name)
            );
          }
          o.struct.registerVfs(!!o.asDefault);
        }
      }
    }
    if(!count) toss("Misuse: installVfs() options object requires at least",
                    "one of:", propList);
    return this;
  };
}/*capdbApiBootstrap.initializers.push()*/);
