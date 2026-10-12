{
  description = "Construct development shell from requirements.txt";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  inputs.pyproject-nix.url = "github:pyproject-nix/pyproject.nix";
  
  inputs.zephyr-sdk.url = "git+https://code.srx.digital/srx/nix-zephyr-sdk";
  
outputs =
    { nixpkgs, pyproject-nix, zephyr-sdk, ... }:
    let
      # Load/parse requirements.txt
      project = pyproject-nix.lib.project.loadRequirementsTxt { 
        projectRoot = ./.; 
        requirements = ./zephyr/scripts/requirements-base.txt ;
      };

      pkgs = nixpkgs.legacyPackages.x86_64-linux;
      python = pkgs.python3;

      pythonEnv =
        # Assert that versions from nixpkgs matches what's described in requirements.txt
        # In projects that are overly strict about pinning it might be best to remove this assertion entirely.
        assert project.validators.validateVersionConstraints { inherit python; } == { };
        (
          # Render requirements.txt into a Python withPackages environment
          pkgs.python3.withPackages (project.renderers.withPackages { inherit python; })
        );

    in
    {
      devShells.x86_64-linux.default = pkgs.mkShell { packages = with pkgs; [ pythonEnv cmake gnumake dtc ninja ]; };
      devShells.x86_64-linux.zsdk = zephyr-sdk.devShells.x86_64-linux.zephyr;
    };
}
