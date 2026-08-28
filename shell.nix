{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  packages = with pkgs; [
    gcc
    cmake
    ninja
    gdb
    clang
    llvm
    valgrind
    linuxPackages.perf
    git
  ];
}
