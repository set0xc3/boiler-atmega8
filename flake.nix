{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
  };

  outputs = {
    self,
    nixpkgs,
    ...
  } @ inputs: let
    system = "x86_64-linux";
    pkgs = nixpkgs.legacyPackages.${system};
  in {
    devShells.${system}.default = pkgs.mkShell {
      buildInputs = [
        pkgs.gdb
        pkgs.usbutils
        pkgs.minicom
        pkgs.avra
        pkgs.avrdude
        pkgs.avrdudess
        pkgs.bear
        pkgs.pkgsCross.avr.buildPackages.gcc
        pkgs.pkgsCross.avr.buildPackages.binutils
        pkgs.pkgsCross.avr.buildPackages.binutils.bintools
        pkgs.pkgsCross.avr.libcCross
      ];

      AVR_PATH = "${pkgs.pkgsCross.avr.libcCross}/avr";
    };
  };
}
