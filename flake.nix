{
  description = "libc-free (freestanding C23) RFC 6455 WebSocket protocol stack SDK";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAll = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in
    {
      packages = forAll (pkgs: rec {
        echo-server = pkgs.runCommand "echo-server" { nativeBuildInputs = [ pkgs.clang ]; } ''
          mkdir -p $out/bin
          clang -std=c23 -ffreestanding -nostdlib -fno-builtin -Wall -Wextra -Werror \
            -O2 -static -I${./include} \
            ${./src}/mem.c ${./src}/mask.c ${./src}/frame.c ${./src}/sha1.c \
            ${./src}/base64.c ${./src}/handshake.c ${./src}/lifecycle.c ${./src}/utf8.c \
            ${./src}/stream.c ${./src}/io_posix.c \
            ${./example}/echo_server.c -o $out/bin/echo-server
        '';
        default = echo-server;
      });

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
