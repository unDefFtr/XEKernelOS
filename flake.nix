{
  description = "XEKernelOS bare-metal x86 development environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { nixpkgs, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
    in
    {
      devShells = nixpkgs.lib.genAttrs systems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          python = pkgs.python3.withPackages (ps: [ ps.pillow ]);
          unifont = pkgs.fetchurl {
            url = "https://unifoundry.com/pub/unifont/unifont-16.0.04/font-builds/unifont-16.0.04.hex.gz";
            hash = "sha256-+cjHgCRT9HvgJncXaurCNC7pbTVPrXomztzOSOaOHZ8=";
          };
        in
        {
          default = pkgs.mkShellNoCC {
            packages = [
              pkgs.gnumake
              pkgs.nasm
              # Avoid host compiler wrappers injecting flags into -target i686-elf.
              pkgs.llvmPackages.clang-unwrapped
              pkgs.llvmPackages.lld
              pkgs.llvmPackages.llvm
              python
              pkgs.qemu
              pkgs.coreutils
            ];

            # Some GUI scripts otherwise default to Windows-specific paths.
            QEMU = "${pkgs.qemu}/bin/qemu-system-i386";
            QEMU_IMG = "${pkgs.qemu}/bin/qemu-img";

            shellHook = ''
              # The Makefile expects this ignored font input beside the scripts.
              if [ -f Makefile ] && [ -d tools ] && [ ! -e tools/unifont.hex.gz ]; then
                ln -sfn ${unifont} tools/unifont.hex.gz
              fi
            '';
          };
        });
    };
}
