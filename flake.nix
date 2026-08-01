{
  description = "Demo Chat standalone app for logos-libp2p-module";

  inputs = {
    logos-libp2p-module.url = "github:logos-co/logos-libp2p-module";
  };

  outputs = { self, logos-libp2p-module }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forEachSystem = f: builtins.listToAttrs (map (system: {
        name = system;
        value = f system;
      }) systems);
    in {
      devShells = forEachSystem (system:
        let
          baseShell = logos-libp2p-module.devShells.${system}.default;
        in {
          default = baseShell.overrideAttrs (old: {
            shellHook = (old.shellHook or "") + ''
              export LOGOS_LIBP2P_MODULE_DIR=${logos-libp2p-module}
            '';
          });
        });
    };
}
