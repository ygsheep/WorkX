{
  description = "Workx development environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs =
    { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShell {
            name = "workx-dev";
            env = {
              # NixOS 上没有 /lib64/ld-linux-x86-64.so.2，跑不了 vcpkg 下载的
              # 通用 Linux 预编译工具（cmake/ninja/git 等）。强制 vcpkg 全部使用
              # Nix 环境自带的二进制，避免 stub-ld: exit 127。
              VCPKG_FORCE_SYSTEM_BINARIES = "1";
              # 供 CMakePresets.json 的 $env{VCPKG_ROOT} 自动定位 vcpkg toolchain。
              VCPKG_ROOT = "${pkgs.vcpkg}/share/vcpkg";
            };
            packages = with pkgs; [
              gcc
              cmake
              ninja
              pkg-config
              vcpkg
              python3
              git
              gnumake
              patch
              gzip
              bzip2
              xz
              unzip
              gnutar
              autoconf
              automake
              libtool
            ];
          };
        });

      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.stdenv.mkDerivation {
            pname = "workx";
            version = "0.2.0";
            src = pkgs.lib.cleanSource self;
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
            buildInputs = [ pkgs.nlohmann_json pkgs.curl ];
            cmakeFlags = [
              "-DWORKX_WITH_TREE_SITTER=ON"
              "-DWORKX_FETCH_GRAMMARS=ON"
              "-DWORKX_BUILD_TESTS=OFF"
              "-DWORKX_BUILD_EXAMPLES=OFF"
            ];
            installPhase = ''
              runHook preInstall
              install -Dm755 build/bin/workx $out/bin/workx
              runHook postInstall
            '';
          };
        });
    };
}
