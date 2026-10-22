{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  nativeBuildInputs = with pkgs; [
    autoconf
    automake
    pkg-config
    gnumake
    gcc
  ];

  buildInputs = with pkgs; [
  ];

  shellHook = ''
    echo "=== recdvb 4K Development Shell ==="
    echo "B61 support is supplied by an installed libaribb61."
    echo ""
    echo "To build recdvb with B61 support:"
    echo "  export PKG_CONFIG_PATH=/path/to/libaribb61/lib/pkgconfig:\$PKG_CONFIG_PATH"
    echo "  ./configure --enable-b61"
    echo "  make"
    echo ""
    echo "  B61:   $(pkg-config --modversion libaribb61 2>/dev/null || echo 'N/A')"
  '';
}
