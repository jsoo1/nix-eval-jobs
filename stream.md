# Debugging nix-eval-jobs with gdb and nix

We use ~nix-eval-jobs~ in CI, it's great!

# Recently solved some issues with our nix-eval-jobs fork for work

## Struggled to find hang

- println helped but only got so far
- problem was in the nix downloading process
- found the problem with gdb

## First time using gdb in anger

- some nix-specific things required to make the process work
- found some really nice nix features!

# Trying to upstream our changes (alterations to our patches needed)

## Preparation

- setup a vm with nixos stable iso for demo (also used for actual bug-hunt!)
- cloned our fork of nix-eval-jobs, added upstream remote
