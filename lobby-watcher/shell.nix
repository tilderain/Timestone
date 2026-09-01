{ pkgs ? import (builtins.fetchGit {
    url = "https://github.com/NixOS/nixpkgs";
    rev = "643809054d65fdd466a63e3155b8c498cb483c04";
  }) {} }:

pkgs.mkShell {
  buildInputs = with pkgs; [
    rustup        # cargo/rustc; `rustup target add x86_64-pc-windows-gnu`
    rustfmt
    clippy
    gcc
    pkgsCross.mingwW64.stdenv.cc   # x86_64-w64-mingw32-gcc — Windows target linker
  ];

  shellHook = ''
    # Build-time requirements: the Steamworks SDK (steamworks-sys locates it
    # via STEAM_SDK_LOCATION) and a stub libpthread.a in the rust-libs dir
    # (nixpkgs' mcfgthread toolchain provides no libpthread; rustc's
    # windows-gnu std links -l:libpthread.a).
    export STEAM_SDK_LOCATION="$HOME/steamworks_sdk"
    export RUSTFLAGS="-C link-arg=-L$HOME/steamworks_sdk/rust-libs"
  '';
}
