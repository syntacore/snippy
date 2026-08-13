{
  stdenv,
  runCommand,
  wrap-buddy,
  llvm-snippy,
  glibc,
  patchelf,
}:

runCommand "snippy-release-tarball-${llvm-snippy.version}"
  {
    bundleName = "snippy-${stdenv.hostPlatform.system}";
    nativeBuildInputs = [
      wrap-buddy
      glibc.bin
      patchelf
    ];
  }
  ''
    mkdir $bundleName && cd $bundleName
    mkdir {bin,lib}

    cp -v ${llvm-snippy.out}/bin/* ./bin/
    cp -v ${wrap-buddy}/lib/wrap-buddy/loader.bin ./lib/loader.bin

    ldd ./bin/* 2>/dev/null | awk '$1~/^\//{print $1} $3~/^\//{print $3}' | sort -u | while IFS= read -r lib; do
      [ -f "$lib" ] && cp -L --remove-destination -v "$lib" ./lib/
    done

    chmod u+w -R ./lib ./bin

    for executable in ./bin/*; do
      patchelf --remove-rpath "$executable"
    done

    # Yes this is very cursed and we are not preserving relative paths at all.
    # Works out fine in practice though.
    find ./lib -type f -name '*.so*' ! -name 'ld-*.so*' | while read -r lib; do
      echo "relativizing shared library '$lib'"
      patchelf --set-rpath '$ORIGIN:.' "$lib"
    done

    interp=$(find ./lib -maxdepth 1 -name 'ld-*.so.*' | head -n1)
    wrap-buddy --paths ./bin --libs ./lib --interpreter "$interp" --relocatable --loader-dir-path ./lib

    mkdir -p share/
    cp -r ${llvm-snippy}/share/examples ./share/

    mkdir $out
    tar --xz -cvf $out/$bundleName.tar.xz .
  ''
