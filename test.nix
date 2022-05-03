let
  pkgs = import ./scratch/pkgs.nix;

  inherit (pkgs) lib;

  testAttrSet = lib.listToAttrs (lib.take 10 (lib.lists.drop 1020
    (lib.mapAttrsToList lib.nameValuePair pkgs)));

  testList = lib.mapAttrsToList (_: x: x) testAttrSet;

  testDrv = pkgs.ripgrep;
in
  testAttrSet
