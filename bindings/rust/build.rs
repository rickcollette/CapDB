use std::env;
use std::path::PathBuf;

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let default_lib = manifest_dir.join("../../build");
    let lib_dir = env::var("CAPDB_LIB_DIR").unwrap_or_else(|_| default_lib.display().to_string());
    println!("cargo:rustc-link-search=native={}", lib_dir);
    println!("cargo:rustc-link-lib=dylib=capdb");
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir);
}
