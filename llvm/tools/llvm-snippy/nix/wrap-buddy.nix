{
  fetchFromGitHub,
  stdenv,
  xxd,
  buildPackages,
}:

stdenv.mkDerivation {
  pname = "wrap-buddy";
  version = "1.0.1-unstable-2026-08-09";

  src = fetchFromGitHub {
    owner = "Mic92";
    repo = "wrap-buddy";
    rev = "1970ac0c4fca82239ad2393ced3833f4ce56a0b9";
    hash = "sha256-DwrmV4oKxN18/NAIicpqPYpxOdIGZpk5qBa2bs82ym4=";
  };

  nativeBuildInputs = [
    xxd
  ];

  makeFlags = [
    "BINDIR=$(out)/bin"
    "LIBDIR=$(out)/lib/wrap-buddy"
    "CXX_FOR_BUILD=${buildPackages.stdenv.cc}/bin/c++"
  ];
}
