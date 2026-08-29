{
  description = "UMVC3Rollback — UMVC3 hook DLL with rollback netplay";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; config.allowUnfree = true; };
        mingw = pkgs.pkgsCross.mingwW64;
      in
      {
        devShells.default = pkgs.mkShell {
          name = "umvc3-dev";
          packages = [
            pkgs.gnumake
            pkgs.zip
            pkgs.ghidra
            mingw.stdenv.cc            # x86_64-w64-mingw32-gcc / -g++
            mingw.stdenv.cc.bintools   # x86_64-w64-mingw32-windres / -strip / -ld
          ];
          shellHook = ''
            echo "UMVC3Rollback dev shell — build with: make release   (or make debug)"
          '';
        };

        packages.timestone = mingw.stdenv.mkDerivation {
          name = "timestone";
          src = pkgs.lib.cleanSource self;
          nativeBuildInputs = [ pkgs.gnumake pkgs.zip ];
          buildPhase = "make clean && make release";
          installPhase = ''
            mkdir -p "$out"
            cp Timestone.asi TimestoneUpdater.exe dinput8.dll Timestone.zip "$out/"
          '';
        };
      });
}
