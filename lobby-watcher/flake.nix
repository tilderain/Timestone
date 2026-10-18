{
  description = "lobby-watcher - headless Timestone lobby watcher (Steamworks lobby-search)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; config.allowUnfree = true; };
      in {
        packages.default = pkgs.rustPlatform.buildRustPackage {
          pname = "lobby-watcher";
          version = "0.1.0";
          src = ./.;
          cargoLock = { lockFile = ./Cargo.lock; };

          # steamworks-sys locates the SDK via STEAM_SDK_LOCATION; the committed
          # sdk/ dir mirrors the redistributable layout it expects.
          preBuild = ''
            export STEAM_SDK_LOCATION=$PWD/sdk
          '';

          postInstall = ''
            mkdir -p $out/lib
            cp sdk/redistributable_bin/linux64/libsteam_api.so $out/lib/libsteam_api.so
            patchelf --set-rpath "$out/lib:$(patchelf --print-rpath $out/bin/lobby-watcher)" $out/bin/lobby-watcher
          '';
        };

        devShells.default = pkgs.mkShell {
          name = "lobby-watcher";
          buildInputs = with pkgs; [ cargo rustc rustfmt clippy ];
          shellHook = ''
            export STEAM_SDK_LOCATION="$PWD/sdk"
            export LD_LIBRARY_PATH="$PWD/sdk/redistributable_bin/linux64:$LD_LIBRARY_PATH"
          '';
        };
      }
    );
}
