{
  description = "GodotPckTool - development shell";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShell {
            name = "godotpcktool-dev";

            packages = with pkgs; [
              # Native build
              cmake
              ninja
              gcc
              binutils       # provides gold linker (-fuse-ld=gold used in release builds)

              # WASM build
              emscripten
              nodejs_22      # emscripten runtime dependency

              # Utilities
              git
            ];

            shellHook = ''
              echo "GodotPckTool dev shell"
              echo ""
              echo "  Native:  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
              echo "  WASM:    emcmake cmake -B build-wasm web/ && cmake --build build-wasm"
            '';
          };
        }
      );
    };
}
