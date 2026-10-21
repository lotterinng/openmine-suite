# vcpkg overlay ports

Ports that replace the ones of the pinned vcpkg release (`VCPKG_TAG` in
`.github/workflows/windows.yml`), registered in `vcpkg-configuration.json`. Each is copied
unchanged from a newer vcpkg release; remove it when the pin moves past that release.

| Port | Copied from | Why |
|---|---|---|
| `json-schema-validator` 2.4.0 | vcpkg 2026.10.16 | 2.3.0#2 (vcpkg 2024.11.16) applies a patch downloaded from a GitHub commit URL whose content, and so its SHA-512, has changed since; 2.4.0 includes that change upstream. Needed by PDAL. |
