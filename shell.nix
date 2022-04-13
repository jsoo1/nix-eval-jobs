{ pkgs ? (
    let
      inherit (builtins) fromJSON readFile;
      flakeLock = fromJSON (readFile ./flake.lock);
      locked = flakeLock.nodes.nixpkgs.locked;
      nixpkgs = assert locked.type == "github"; builtins.fetchTarball {
        url = "https://github.com/${locked.owner}/${locked.repo}/archive/${locked.rev}.tar.gz";
        sha256 = locked.narHash;
      };
    in
    import nixpkgs { }
  )
, srcDir ? null
}:

(pkgs.callPackage ./default.nix {
  inherit srcDir;
  nix = pkgs.nix;
}).overrideAttrs (old: {

  src = null;

  nativeBuildInputs = old.nativeBuildInputs ++ [

    pkgs.editorconfig-checker

    pkgs.nixpkgs-fmt

    (pkgs.python3.withPackages (ps: [
      ps.pytest
    ]))

  ];

  shellHook = ''
    export NIX_DEBUG_INFO_DIRS="${pkgs.curl.debug}/lib/debug:${pkgs.nix.debug}/lib/debug''${NIX_DEBUG_INFO_DIRS:+:$NIX_DEBUG_INFO_DIRS}"
  '';
})
