//! Windows resources for both exes: icon, version info, and a manifest (per-monitor DPI v2, common controls 6,
//! UTF-8 code page, Windows 10/11).

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=assets/meowbox.ico");
    tray_icons();
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }
    let mut res = winresource::WindowsResource::new();
    if std::path::Path::new("assets/meowbox.ico").exists() {
        res.set_icon("assets/meowbox.ico");
    }
    res.set("ProductName", "MeowBox Agent")
        .set("FileDescription", "MeowBox Agent")
        .set("CompanyName", "MeowBox")
        .set("LegalCopyright", "MIT License")
        .set_manifest(MANIFEST);
    if let Err(e) = res.compile() {
        println!("cargo:warning=Windows resources not embedded: {e}");
    }
}

/// The hand-drawn tray icons (assets/icons/cat-<state>-<size>.png, see assets/make_art.py) as straight RGBA in
/// OUT_DIR, listed in OUT_DIR/tray_icons.rs, so the exe needs no PNG decoder.
fn tray_icons() {
    println!("cargo:rerun-if-changed=assets/icons");
    let out = std::path::PathBuf::from(std::env::var("OUT_DIR").expect("OUT_DIR"));
    let mut list = String::from("pub const TRAY_ICONS: &[(&str, u32, &[u8])] = &[\n");
    let mut entries: Vec<_> = std::fs::read_dir("assets/icons")
        .map(|rd| rd.flatten().map(|e| e.path()).filter(|p| p.extension().is_some_and(|x| x == "png")).collect())
        .unwrap_or_default();
    entries.sort();
    for path in entries {
        let stem = path.file_stem().unwrap().to_string_lossy().into_owned();
        let Some((state, size)) = stem.strip_prefix("cat-").and_then(|s| s.rsplit_once('-')) else { continue };
        let Ok(size) = size.parse::<u32>() else { continue };
        let mut dec = png::Decoder::new(std::io::BufReader::new(std::fs::File::open(&path).expect("icon")));
        dec.set_transformations(png::Transformations::EXPAND | png::Transformations::ALPHA);
        let mut reader = dec.read_info().expect("png header");
        let mut buf = vec![0; reader.output_buffer_size().expect("png size")];
        let info = reader.next_frame(&mut buf).expect("png data");
        assert!(
            info.width == size && info.height == size && info.color_type == png::ColorType::Rgba,
            "{}: want {size}x{size} RGBA",
            path.display()
        );
        let raw = out.join(format!("{stem}.rgba"));
        std::fs::write(&raw, &buf[..info.buffer_size()]).expect("write icon");
        list.push_str(&format!("    ({state:?}, {size}, include_bytes!({:?})),\n", raw.display().to_string()));
    }
    list.push_str("];\n");
    std::fs::write(out.join("tray_icons.rs"), list).expect("write tray_icons.rs");
}

const MANIFEST: &str = r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="MeowBox.Agent" version="1.0.0.0"/>
  <dependency>
    <dependentAssembly>
      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0"
        processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>
    </dependentAssembly>
  </dependency>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
    </application>
  </compatibility>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
      <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
    </windowsSettings>
  </application>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security><requestedPrivileges><requestedExecutionLevel level="asInvoker" uiAccess="false"/></requestedPrivileges></security>
  </trustInfo>
</assembly>
"#;
