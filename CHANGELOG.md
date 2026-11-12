# Changelog

## 0.1.3

- Package binaries in the OBS directory layout for extract-and-overwrite installation.
- Remove the installation script and simplify documentation and release assets.

## 0.1.2

- Place the worker beside OBS DLLs in `bin/64bit` to resolve dependencies.
- Validate capture and worker recovery on OBS 33.0.1.

## 0.1.1

- Fix worker dependency lookup.

## 0.1.0

- Match windows by executable name and size, composite floating windows and capture process audio.
- Isolate capture in worker processes with automatic recovery.
