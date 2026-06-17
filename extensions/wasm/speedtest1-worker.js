'use strict';
(function(){
  let speedtestJs = 'speedtest1.js';
  const urlParams = new URL(globalThis.location.href).searchParams;
  if(urlParams.has('capdb.dir')){
    speedtestJs = urlParams.get('capdb.dir') + '/' + speedtestJs;
  }
  importScripts(speedtestJs);
  /**
     If this build includes WASMFS, this function initializes it and
     returns the name of the dir on which OPFS is mounted, else it
     returns an empty string.
  */
  const wasmfsDir = function f(wasmUtil){
    if(undefined !== f._) return f._;
    const pdir = '/opfs';
    if( !globalThis.FileSystemHandle
        || !globalThis.FileSystemDirectoryHandle
        || !globalThis.FileSystemFileHandle){
      return f._ = "";
    }
    try{
      if(0===wasmUtil.xCallWrapped(
        'capdb_wasm_init_wasmfs', 'i32', ['string'], pdir
      )){
        return f._ = pdir;
      }else{
        return f._ = "";
      }
    }catch(e){
      // capdb_wasm_init_wasmfs() is not available
      return f._ = "";
    }
  };
  wasmfsDir._ = undefined;

  const mPost = function(msgType,payload){
    postMessage({type: msgType, data: payload});
  };

  const App = Object.create(null);
  App.logBuffer = [];
  const logMsg = (type,msgArgs)=>{
    const msg = msgArgs.join(' ');
    App.logBuffer.push(msg);
    mPost(type,msg);
  };
  const log = (...args)=>logMsg('stdout',args);
  const logErr = (...args)=>logMsg('stderr',args);
  const realSahName = 'opfs-sahpool-speedtest1';

  const runSpeedtest = async function(cliFlagsArray){
    const scope = App.wasm.scopedAllocPush();
    const dbFile = App.pDir+"/speedtest1.capdb";
    try{
      const argv = [
        "speedtest1.wasm", ...cliFlagsArray, dbFile
      ];
      App.logBuffer.length = 0;
      const ndxSahPool = argv.indexOf('opfs-sahpool');
      if(ndxSahPool>0){
        argv[ndxSahPool] = realSahName;
        log("Updated argv for opfs-sahpool: --vfs",realSahName);
      }
      mPost('run-start', [...argv]);
      if(App.capdb.installOpfsSAHPoolVfs
         && !App.capdb.$SAHPoolUtil
         && ndxSahPool>0){
        log("Installing opfs-sahpool as",realSahName,"...");
        await App.capdb.installOpfsSAHPoolVfs({
          name: realSahName,
          initialCapacity: 3,
          clearOnInit: true,
          verbosity: 2
        }).then(PoolUtil=>{
          log("opfs-sahpool successfully installed as",PoolUtil.vfsName);
          App.capdb.$SAHPoolUtil = PoolUtil;
          //console.log("capdb.oo1.OpfsSAHPoolDb =", App.capdb.oo1.OpfsSAHPoolDb);
        });
      }
      App.wasm.xCall('wasm_main', argv.length,
                     App.wasm.scopedAllocMainArgv(argv));
      log("WASM heap size:",App.wasm.heap8().byteLength,"bytes");
      log("WASM pointer size:",App.wasm.ptr.size);

    }catch(e){
      mPost('error',e.message);
    }finally{
      App.wasm.scopedAllocPop(scope);
      mPost('run-end', App.logBuffer.join('\n'));
      App.logBuffer.length = 0;
    }
  };

  globalThis.onmessage = function(msg){
    msg = msg.data;
    switch(msg.type){
        case 'run':
          runSpeedtest(msg.data || [])
            .catch(e=>mPost('error',e));
          break;
        default:
          logErr("Unhandled worker message type:",msg.type);
          break;
    }
  };

  const EmscriptenModule = {
    print: log,
    printErr: logErr,
    setStatus: (text)=>mPost('load-status',text)
  };
  log("Initializing speedtest1 module...");
  globalThis.capdbInitModule.__isUnderTest = true;
  globalThis.capdbInitModule(EmscriptenModule).then(async (capdb)=>{
    const S = globalThis.S = App.capdb = capdb;
    log("Loaded speedtest1 module. Setting up...");
    App.pDir = wasmfsDir(S.wasm);
    App.wasm = S.wasm;
    log("WASM heap size:",capdb.wasm.heap8().byteLength,"bytes");
    log("WASM pointer size:",capdb.wasm.ptr.size);
    //if(App.pDir) log("Persistent storage:",pDir);
    //else log("Using transient storage.");
    mPost('ready',true);
    log("Registered VFSes:", ...S.capi.capdb_js_vfs_list());
  }).catch(e=>{
    logErr(e);
  });
})();
