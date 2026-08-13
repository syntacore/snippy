# The dev shell definitions for nix-shell or nix develop -f shell.nix

let
  sources = import ./nix/pins;
  lib = import (sources.nixpkgs + "/lib");
in

{
  stdenvAttrPath ? "stdenv",
}:

let
  scope = import ./default.nix { inherit stdenvAttrPath; };
  # (sic)
  pkgs = scope.callPackage ({ pkgs }: pkgs) { };
in

scope.llvm-snippy.overrideAttrs (
  finalAttrs: prevAttrs: {
    src = null; # So that we don't copy sources to store.
    nativeBuildInputs =
      prevAttrs.nativeBuildInputs
      ++ (with pkgs; [
        # Whatever other dependencies you might want in the devshell.
        npins
      ]);
  }
)
