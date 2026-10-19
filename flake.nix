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
      flake-utils.lib.eachSystem allSystems (system: let
        pkgs = import nixpkgs { inherit system; };
        inherit (pkgs) lib;

        devShellInputs = with pkgs; [
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

        # Only what the firmware build itself needs on PATH.
        buildTools = with pkgs; [
          bison
          cmake
          flex
          gperf
          git
          ninja
          (python313.withPackages (ps:
            with ps; [
              west
              pyelftools
            ]))
        ];

        # The fork's own sources, and nothing else. zephyr/, modules/, .west/ and
        # the build directories are west output: about 2 GB of git checkout that
        # used to be copied into the store on every single build.
        relOf = path: lib.removePrefix "/" (lib.removePrefix (toString ./.) (toString path));

        westOutputPaths = [
          "zephyr"
          "modules"
          ".west"
          "build"
          "bootloader"
          "tools"
          "zmk-config"
          "app/build"
        ];

        # nix out-links are symlinks into the store; letting them into a source
        # tree would drag the whole zephyr checkout along with them
        isOutLink = rel: rel == "result" || lib.hasPrefix "result/" rel || lib.hasPrefix "result-" rel;

        notWestOutput = extra: path: _type: let
          rel = relOf path;
          skip = westOutputPaths ++ extra;
        in
          !lib.any (prefix: rel == prefix || lib.hasPrefix (prefix + "/") rel) skip
          && !isOutLink rel;

        appSource = lib.cleanSourceWith {
          src = lib.cleanSource ./.;
          filter = notWestOutput [];
        };

        # The host tests are not part of the keyboard, so changing a test must
        # not cost a rebuild of the firmware.
        firmwareSource = lib.cleanSourceWith {
          src = lib.cleanSource ./.;
          filter = notWestOutput [ "tests" ];
        };

        # "west update" only reads the manifest, so the module clone depends on
        # this one file and nothing else in the fork.
        westManifestSource = lib.cleanSourceWith {
          src = lib.cleanSource ./.;
          filter = path: _type: let
            rel = relOf path;
          in
            rel == "." || rel == "app" || rel == "app/west.yml";
        };

        # Zephyr and every west module, cloned once and then cached in the store.
        # "west update" needs the network and nix only grants that to a
        # fixed-output derivation, so this is the only place an outputHash is
        # needed, and it only has to be re-blessed when app/west.yml changes.
        westModules = pkgs.stdenv.mkDerivation {
          pname = "keychron-zmk-west-modules";
          version = "1.0.0";
          src = westManifestSource;

          nativeBuildInputs = [
            pkgs.cacert
            pkgs.git
            (pkgs.python313.withPackages (ps:
              with ps; [
                west
              ]))
          ];

          # west and git want a writable home plus a ca bundle for the clones
          HOME = "/tmp";
          SSL_CERT_FILE = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";
          GIT_SSL_CAINFO = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";

          outputHashMode = "recursive";
          outputHashAlgo = "sha256";
          outputHash = "sha256-XqpJw+ybAOQ5v4D0IO+O9jGm+HbEORTBvW0ciPexTLU=";

          # stdenv's fixup phase rewrites every "#!/bin/sh" it finds into a store
          # path, and a fixed-output output is not allowed to reference the store.
          # These are plain source trees, so there is nothing to fix up anyway.
          dontFixup = true;
          dontPatchShebangs = true;

          buildPhase = ''
            runHook preBuild
            rm -rf .west zephyr modules
            west init -l app
            west update
            # a manifest with every import resolved and every revision pinned to
            # a sha, so the firmware build does not need any git checkouts.
            # --active-only leaves out the projects west update does not clone
            # (the optional and babblesim groups), which --freeze would refuse.
            west manifest --freeze --active-only > frozen-west.yml
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p $out
            cp -a zephyr $out/zephyr
            cp -a modules $out/modules
            cp frozen-west.yml $out/west.yml
            # west only needs the trees; the git history is hundreds of MB and
            # its packs and reflogs are not reproducible, which a fixed-output
            # hash cannot tolerate
            find $out -name .git -type d -prune -exec rm -rf {} +
            runHook postInstall
          '';
        };

        # An ordinary derivation: no network, no outputHash, so editing the
        # firmware source just recompiles instead of re-blessing a hash.
        buildFirmwareForKeyboard = { board, shield, ... }: pkgs.stdenv.mkDerivation {
          pname = "${board}-${shield}-firmware";
          version = "1.0.0";
          src = firmwareSource;

          nativeBuildInputs = buildTools;
          buildInputs = [ zephyr-sdk.packages.${system}.zephyr-sdk ];

          ZEPHYR_TOOLCHAIN_VARIANT = "zephyr";
          ZEPHYR_SDK_INSTALL_DIR = "${zephyr-sdk.packages.${system}.zephyr-sdk}";
          HOME = "/tmp";

          postUnpack = ''
            cp -a ${westModules}/zephyr $sourceRoot/zephyr
            cp -a ${westModules}/modules $sourceRoot/modules
            # the frozen manifest replaces the import, so west build works from
            # plain directories with no git checkouts and no network
            cp ${westModules}/west.yml $sourceRoot/app/west.yml
            chmod -R u+w $sourceRoot/zephyr $sourceRoot/modules $sourceRoot/app
          '';

          # west does the configuring, stdenv must not run cmake itself
          configurePhase = ''
            runHook preConfigure
            runHook postConfigure
          '';

          buildPhase = ''
            runHook preBuild
            west init -l app
            west build -s app -d app/build -b ${board} -- -DSHIELD=${shield}
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p $out
            cp app/build/zephyr/zmk.bin $out/${shield}.bin
            for ext in elf map hex uf2; do
              if [ -f "app/build/zephyr/zmk.$ext" ]; then
                cp "app/build/zephyr/zmk.$ext" "$out/${shield}.$ext"
              fi
            done
            runHook postInstall
          '';
        };

        # The snake rules are plain C, so the host test that guards them builds
        # like anything else: nix build .#packages.x86_64-linux.snakeTest
        snakeTest = pkgs.stdenv.mkDerivation {
          pname = "keychron-snake-test";
          version = "1.0.0";
          src = appSource;

          nativeBuildInputs = [ pkgs.gcc ];

          buildPhase = ''
            runHook preBuild
            gcc -std=c11 -Wall -Wextra -O2 -I app/src/rgb -o snake_sim tests/snake_sim.c
            runHook postBuild
          '';

          doCheck = true;
          checkPhase = ''
            runHook preCheck
            ./snake_sim > snake-report.txt
            runHook postCheck
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p $out/bin
            cp snake_sim $out/bin/snake_sim
            cp snake-report.txt $out/report.txt
            runHook postInstall
          '';

          meta.mainProgram = "snake_sim";
        };
       in
        {
          packages = let
            firmwares = builtins.mapAttrs (name: attr:
              buildFirmwareForKeyboard attr
            ) supportedKeyboards;
          in
            firmwares // {
              inherit westModules snakeTest;
              default = firmwares.keychron_q6_ultra_iso;
            };
          devShells = {
            default = zephyr-sdk.devShells.x86_64-linux.zephyr;
          };
        }
      );
}
