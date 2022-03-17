{
  description = "Hydra's builtin hydra-eval-jobs as a standalone";

  inputs.nixpkgs.url = "github:awakesecurity/nixpkgs/awake-20.09";
  inputs.flake-utils.url = "github:numtide/flake-utils";

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        drvArgs = { srcDir = self; nix = pkgs.nix; };
      in
      rec {
        packages.nix-eval-jobs = pkgs.callPackage ./default.nix drvArgs;

        checks =
          let
            mkVariant = nix: (packages.nix-eval-jobs.override {
              inherit nix;
            }).overrideAttrs (_: {
              name = "nix-eval-jobs-${nix.version}";
              inherit (nix) version;
            });
          in
          {

            editorconfig = pkgs.runCommand "editorconfig-check"
              {
                nativeBuildInputs = [
                  pkgs.editorconfig-checker
                ];
              } ''
              editorconfig-checker ${self}
              touch $out
            '';

            nixpkgs-fmt = pkgs.runCommand "fmt-check"
              {
                nativeBuildInputs = [
                  pkgs.nixpkgs-fmt
                ];
              } ''
              nixpkgs-fmt --check .
              touch $out
            '';

            build = mkVariant pkgs.nix;
            build-unstable = mkVariant pkgs.nixUnstable;
          };

        defaultPackage = self.packages.${system}.nix-eval-jobs;
        devShell = pkgs.callPackage ./shell.nix drvArgs;

      }
    );
}
