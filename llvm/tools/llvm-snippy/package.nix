{
  stdenv,
  lib,
  cmake,
  pkg-config,
  ninja,
  mold,
  python3,
  ruby,
  sphinx,
  lit,
  rvmi,
  riscv-isa-sim,
  versionCheckHook,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "llvm-snippy";
  version = "3.1.0";

  src = lib.fileset.toSource rec {
    root = ../../..;
    fileset = lib.fileset.difference (lib.fileset.unions (
      lib.map (x: root + "/${x}") [
        "LICENSE.TXT"
        "cmake"
        "lld"
        "llvm/CMakeLists.txt"
        "llvm/LICENSE.TXT"
        "llvm/cmake"
        "llvm/include"
        "llvm/lib"
        "llvm/projects"
        "llvm/test/lit.cfg.py"
        "llvm/test/lit.site.cfg.py.in"
        "llvm/test/Unit"
        "llvm/test/CMakeLists.txt"
        "llvm/test/tools/llvm-snippy"
        "llvm/tools"
        "llvm/unittests"
        "llvm/utils"
        "third-party"
        "libunwind"
      ]
    )) (lib.fileset.fileFilter (file: file.hasExt "nix") root);
  };

  sourceRoot = "source/llvm";

  outputs = [
    "out"
    "doc"
    "man"
  ];

  cmakeFlags = [
    (lib.cmakeFeature "LLVM_USE_LINKER" "mold")
    (lib.cmakeFeature "LLVM_ENABLE_PROJECTS" "lld")
    (lib.cmakeBool "LLVM_BUILD_TESTS" finalAttrs.finalPackage.doCheck)
    (lib.cmakeBool "LLVM_ENABLE_ASSERTIONS" true)
    (lib.cmakeFeature "LLVM_TARGETS_TO_BUILD" "RISCV;AArch64")
    (lib.cmakeBool "LLVM_BUILD_SNIPPY" true)
    (lib.cmakeBool "LLVM_ENABLE_SPHINX" true)
    (lib.cmakeBool "LLVM_INCLUDE_BENCHMARKS" false)
    (lib.cmakeBool "LLVM_INCLUDE_EXAMPLES" false)
    (lib.cmakeBool "LLVM_INCLUDE_RUNTIMES" false)
    (lib.cmakeBool "LLVM_INCLUDE_DOCS" false)
    (lib.cmakeFeature "RISCVModelSpike_DIR" "${lib.getLib riscv-isa-sim}/lib")
  ];

  buildPhase = ''
    cmake --build . --target llvm-snippy llvm-ie docs-llvm-snippy-latex docs-llvm-snippy-html docs-llvm-snippy-man
  '';

  nativeBuildInputs = [
    cmake
    pkg-config
    (python3.withPackages (ps: with ps; [ myst-parser ]))
    ninja
    sphinx
    mold
  ];

  strictDeps = true;

  buildInputs = [
    rvmi
    riscv-isa-sim
  ];

  nativeCheckInputs = [
    lit
    ruby
  ];

  preCheck = ''
    patchShebangs ..
  '';
  checkPhase = ''
    runHook preCheck
    export LIT_OPTS="-v --no-progress-bar"
    cmake --build . --target check-llvm-tools-llvm-snippy
    export LIT_OPTS="-v --no-progress-bar -Dsnippy-test-model=spike"
    cmake --build . --target check-llvm-tools-llvm-snippy
    runHook postCheck
  '';

  installPhase = ''
    for component in llvm-snippy llvm-snippy-man-pages; do
      cmake --install . --component "$component" --strip
    done

    mkdir -p $doc
    cp -r ./tools/llvm-snippy/docs/{html,latex} $doc/
    ln -s ${lib.getLib riscv-isa-sim}/lib/libRISCVModel.so $out/bin/riscv-spike-plugin.so
  '';

  doCheck = stdenv.buildPlatform.canExecute stdenv.hostPlatform;

  nativeInstallCheckInputs = [ versionCheckHook ];

  doInstallCheck = true;

  meta.mainProgram = "llvm-snippy";
})
