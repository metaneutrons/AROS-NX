#!/usr/bin/env python3
"""Run pinned esptool 5.5.0 with one whole-flash attempt."""
from importlib import metadata


PINNED_VERSION = "5.5.0"
EXPECTED_DEFAULT_ATTEMPTS = 2


def configure_single_write_attempt(esptool_module, loader_module):
    """Fail closed unless the pinned loader exposes the reviewed retry knob."""
    try:
        installed_version = metadata.version("esptool")
    except metadata.PackageNotFoundError as error:
        raise RuntimeError("the pinned esptool distribution is unavailable") from error
    if (installed_version != PINNED_VERSION
            or getattr(esptool_module, "__version__", None) != PINNED_VERSION):
        raise RuntimeError("hs recovery requires esptool 5.5.0 exactly")

    loader = getattr(loader_module, "ESPLoader", None)
    if getattr(loader, "WRITE_FLASH_ATTEMPTS", None) != EXPECTED_DEFAULT_ATTEMPTS:
        raise RuntimeError("unexpected esptool 5.5.0 write-attempt implementation")
    loader.WRITE_FLASH_ATTEMPTS = 1
    if loader.WRITE_FLASH_ATTEMPTS != 1:
        raise RuntimeError("could not disable whole-flash retry")


def main():
    import esptool
    from esptool import loader

    configure_single_write_attempt(esptool, loader)
    esptool._main()


if __name__ == "__main__":
    main()
