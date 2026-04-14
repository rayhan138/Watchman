fn main() {
    #[cfg(target_os = "windows")]
    {
        cc::Build::new()
            .cpp(true)
            .file("native/taskbar_embed.cpp")
            .flag_if_supported("/std:c++17")
            .compile("taskbar_embed_native");

        let out_dir = std::env::var("OUT_DIR").expect("OUT_DIR not set");
        println!("cargo:rustc-link-search=native={out_dir}");
        println!("cargo:rustc-link-lib=static=taskbar_embed_native");
        println!(
            "cargo:rustc-link-arg-bins={}\\taskbar_embed_native.lib",
            out_dir
        );
        println!("cargo:rerun-if-changed=native/taskbar_embed.cpp");
    }

    tauri_build::build()
}
