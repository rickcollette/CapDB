/*
  2022-05-22

  The author disclaims copyright to this source code.  In place of a
  legal notice, here is a blessing:

  *   May you do good and not evil.
  *   May you find forgiveness for yourself and forgive others.
  *   May you share freely, never taking more than you give.

  ***********************************************************************

  A basic test script for capdb-api.js. This file must be run in
  main JS thread and capdb.js must have been loaded before it.
*/
import capdbInitModule from './jswasm/capdb-wasmfs.mjs';
//console.log('capdbInitModule =',capdbInitModule);
const toss = function(...args){throw new Error(args.join(' '))};
const log = console.log.bind(console),
      warn = console.warn.bind(console),
      error = console.error.bind(console);

const stdout = log;
const stderr = error;

const test1 = function(db){
  db.exec("create table if not exists t(a);")
    .transaction(function(db){
      db.prepare("insert into t(a) values(?)")
        .bind(new Date().getTime())
        .stepFinalize();
      stdout("Number of values in table t:",
             db.selectValue("select count(*) from t"));
    });
};

const runTests = function(capdb){
  const capi = capdb.capi,
        oo = capdb.oo1,
        wasm = capdb.wasm;
  stdout("Loaded module:",capdb);
  stdout("Loaded capdb:",capi.capdb_libversion(), capi.capdb_sourceid());
  const persistentDir = capi.capdb_wasmfs_opfs_dir();
  if(persistentDir){
    stdout("Persistent storage dir:",persistentDir);
  }else{
    stderr("No persistent storage available.");
  }
  const startTime = performance.now();
  let db;
  try {
    db = new oo.DB(persistentDir+'/foo.db');
    stdout("DB filename:",db.filename);
    const banner1 = '>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>',
          banner2 = '<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<';
    [
      test1
    ].forEach((f)=>{
      const n = performance.now();
      stdout(banner1,"Running",f.name+"()...");
      f(db, capdb);
      stdout(banner2,f.name+"() took ",(performance.now() - n),"ms");
    });
  }finally{
    if(db) db.close();
  }
  stdout("Total test time:",(performance.now() - startTime),"ms");
};

capdbInitModule().then(runTests);
