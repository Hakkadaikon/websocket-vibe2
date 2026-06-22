{
  description = "libc-free (freestanding C23) RFC 6455 WebSocket protocol stack SDK";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAll = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in
    {
      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = [
            pkgs.clang     # C23 + clang-tidy / clang-format
            pkgs.just
            pkgs.lizard    # cyclomatic complexity
            pkgs.python3   # lizard runtime
          ];
        };
      });
    };
}
