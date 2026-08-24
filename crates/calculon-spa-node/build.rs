//! Builds the narrow C bridge to PipeWireAO's header-only transport helpers.

use std::env;

fn main() {
    println!("cargo:rerun-if-changed=src/latest.c");
    let spa = pkg_config::Config::new()
        .cargo_metadata(false)
        .probe("libspa-ao-0.2")
        .expect("PipeWireAO SPA headers are required");
    let mut build = cc::Build::new();
    build.file("src/latest.c").include("../../include");
    println!("cargo:rerun-if-env-changed=PIPEWIREAO_SPA_INCLUDE_DIR");
    if let Some(include) = env::var_os("PIPEWIREAO_SPA_INCLUDE_DIR") {
        build.include(include);
    }
    for include in spa.include_paths {
        build.include(include);
    }
    build.warnings(true).compile("calculon_spa_latest");
}
