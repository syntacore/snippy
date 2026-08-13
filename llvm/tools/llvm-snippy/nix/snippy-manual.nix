{
  stdenv,
  texliveBasic,
  llvm-snippy,
  dejavu_fonts,
}:

stdenv.mkDerivation {
  pname = "snippy-manual";
  src = "${llvm-snippy.doc}/latex";
  version = llvm-snippy.version;

  nativeBuildInputs = [
    (texliveBasic.withPackages (
      tp: with tp; [
        latexmk
        xetex
        cmap
        fontspec
        polyglossia
        fncychap
        xcolor
        float
        wrapfig
        capt-of
        framed
        needspace
        fancyvrb
        upquote
        tabulary
        varwidth
        booktabs
        parskip
        titlesec
        tocloft
        metafont
        pict2e
        zapfchan
      ]
    ))
    dejavu_fonts
  ];

  installPhase = ''
    mkdir $out
    cp LLVMSnippy.pdf $out/snippy_guide.pdf
  '';
}
