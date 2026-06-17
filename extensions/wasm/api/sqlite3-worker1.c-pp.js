//#if not omit-oo1
/*
  2022-05-23

  The author disclaims copyright to this source code.  In place of a
  legal notice, here is a blessing:

  *   May you do good and not evil.
  *   May you find forgiveness for yourself and forgive others.
  *   May you share freely, never taking more than you give.

  ***********************************************************************

  This is a JS Worker file for the main capdb api. It loads
  capdb.js, initializes the module, and postMessage()'s a message
  after the module is initialized:

  {type: 'capdb-api', result: 'worker1-ready'}

  This seemingly superfluous level of indirection is necessary when
  loading capdb.js via a Worker. Instantiating a worker with new
  Worker("sqlite.js") will not (cannot) call capdbInitModule() to
  initialize the module due to a timing/order-of-operations conflict
  (and that symbol is not exported in a way that a Worker loading it
  that way can see it).  Thus JS code wanting to load the capdb
  Worker-specific API needs to pass _this_ file (or equivalent) to the
  Worker constructor and then listen for an event in the form shown
  above in order to know when the module has completed initialization.

  This file accepts a URL arguments to adjust how it loads capdb.js:

  - `capdb.dir`, if set, treats the given directory name as the
    directory from which `capdb.js` will be loaded.
*/
//#if target:es6-bundler-friendly
import capdbInitModule from './capdb-bundler-friendly.mjs';
//#elif target:es6-module
import capdbInitModule from './capdb.mjs';
//#else
"use strict";
{
  const urlParams = globalThis.location
        ? new URL(globalThis.location.href).searchParams
        : new URLSearchParams();
  let theJs = 'capdb.js';
  if(urlParams.has('capdb.dir')){
    theJs = urlParams.get('capdb.dir') + '/' + theJs;
  }
  //console.warn("worker1 theJs =",theJs);
  importScripts(theJs);
}
//#/if
capdbInitModule().then(capdb => capdb.initWorker1API());
//#else
/* Built with the omit-oo1 flag. */
//#/if if not omit-oo1
