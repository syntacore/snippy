{
  lib,
  sources,
  self, # This is the scope fixpoint.
  ...
}@args:

let
  inherit (self) callPackage;
in

# Note that this is strict in the attrset spine.
# This can be avoided if needed if we can define the package names without fetching.
# The benefit of doing so is a bit dubious though since all dependencies will
# be forced regardless.
lib.mergeAttrsList (
  lib.map (path: import path { inherit callPackage; }) [
    ./scope.nix
    (/. + (builtins.unsafeDiscardStringContext sources.rvmi) + "/nix/scope.nix")
  ]
)
