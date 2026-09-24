fn main() {
    let crate_dir = std::env::var("CARGO_MANIFEST_DIR").unwrap_or_default();
    if let Ok(b) = cbindgen::generate(&crate_dir) {
        b.write_to_file("include/skymp_wire.h");
    }
    println!("cargo:rerun-if-changed=src/lib.rs");
}
