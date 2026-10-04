# Packaging runbook (summary)

- Windows: `windeployqt` over the Release binary, then NSIS/MSI (signing in CI).
- macOS: `macdeployqt` bundle + notarization (protected env secrets only).
- Linux: AppImage/tarball with Qt base deploy; verify launch on clean image.
- Every artifact must contain the binary + `styles` + `migrations` resources
  and pass `--smoke-test` after install/mount.
