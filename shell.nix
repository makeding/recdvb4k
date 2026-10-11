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
    pcsclite
    openssl
    asio
  ] ++ lib.optionals stdenv.isLinux [
    pcsc-lite  # pcscd daemon on Linux
  ];

  shellHook = ''
    echo "=== ACAS/4K Development Shell ==="
    echo "Dependencies: pcsclite, openssl, asio"
    echo ""
    echo "To build recdvb with ACAS/B61 support:"
    echo "  ./configure --enable-b61 --with-asio-include=${pkgs.asio}/include"
    echo "  make"
    echo ""
    echo "Dependencies available:"
    echo "  PCSC:  $(pkg-config --libs libpcsclite 2>/dev/null || echo 'N/A')"
    echo "  SSL:   $(pkg-config --libs openssl 2>/dev/null || echo 'N/A')"
  '';
}
