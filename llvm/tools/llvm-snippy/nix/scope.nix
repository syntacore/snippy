{ callPackage, ... }: {
  llvm-snippy = callPackage ../package.nix { };
  snippy-manual = callPackage ./snippy-manual.nix { };
  wrap-buddy = callPackage ./wrap-buddy.nix { };
  snippy-release-tarball = callPackage ./snippy-release-tarball.nix { };
}
