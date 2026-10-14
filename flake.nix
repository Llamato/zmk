{
  description = "keychron zmk fork";


  inputs = {
    nixpkgs = {
      url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    };
    flake-utils = {
      url = "github:numtide/flake-utils";
    };
    zephyr-sdk = {
      url = "git+https://code.srx.digital/srx/nix-zephyr-sdk";
    };
  };
  
outputs =
    { nixpkgs, flake-utils, zephyr-sdk, ... }: 
    let 
      linuxSystems = [
        "x86_64-linux"
        "aarch64-linux"
        "riscv64-linux"
      ];
      darwinSystems = [
        "aarch64-darwin"
      ];
      allSystems = linuxSystems ++ darwinSystems;

      supportedKeyboards = {
        keychron_q6_ultra_iso = {
          board = "keychron";
          shield = "keychron_q6_ultra_iso";
        };
      };
    in 
      flake-utils.lib.eachSystem allSystems ( system: let
        pkgs = import nixpkgs { inherit system; };
        zmkSrc = pkgs.fetchFromGitHub {
          owner = "llamato";
          repo = "zmk";
          rev = "master";
          hash = "sha256-ubD24viHiNa+YhjuyqkcenynvI+/po9W/Shzh7NJKIQ=";
        };
        buildInputs = with pkgs; [
          # C/CPP
          bear
          bison
          ccache
          clang
          clang-tools
          clangStdenv
          cmake
          cmake-format
          cmake-language-server
          dfu-util
          flex
          gdb
          gnumake
          gperf
          gtest
          libffi
          libusb1
          ncurses
          ninja

          # Zephyr
          (python313.withPackages (ps:
            with ps; [
              west
              pyelftools
            ]))
          zephyr-sdk.packages.${system}.zephyr-sdk

          # ESP
          espup
          esptool

          # Nix
          git
        ];

        buildFirmwareForKeyboard = { board, shield, ... }: pkgs.stdenv.mkDerivation {
          inherit buildInputs;
          pname = "${board}-${shield}-firmware";
          version = "1.0.0";
          src = zmkSrc;

          configurePhase = ''

          '';

          buildPhase = ''
            rm -rf .west
            rm -rf zephyr modules
            rm -rf app/build
            west init -l app
            west update #broken
            cd app
            west build -p -b ${board} -- -DSHIELD=${shield}
          '';
        };
       in
        {
          packages = builtins.mapAttrs (name: attr:
            buildFirmwareForKeyboard attr
          ) supportedKeyboards;
          devShells = {
            default = zephyr-sdk.devShells.x86_64-linux.zephyr;
          };
        }
      );
}
