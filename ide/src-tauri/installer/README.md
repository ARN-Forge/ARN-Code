# ARN Windows setup artwork

Design: ink navy `#101820`, orange `#FF9820`, white content area, Segoe UI.
Arny uses the same block geometry as the CLI and the app icon. Keep artwork
free of version numbers so upgrades cannot display stale branding.

- `sidebar.bmp`: 328 × 628, 24-bit RGB, 2× NSIS welcome/finish artwork.
- `sidebar.png`: reviewable copy of the same artwork.
- `header.bmp`: 150 × 57, 24-bit RGB, right-aligned page header branding.
- `branding.nsh`: localized welcome/finish content and native UI styling.
- `runtime/`: generated, ignored payload; contains no provider credentials.

Regenerate art with `scripts/generate-installer-art.ps1` on Windows. Build
with `npm run installer:build` from `ide`. Do not fork the complete Tauri NSIS
template: its process handling, upgrade detection, WebView2 bootstrap and
uninstallation logic remain maintained by Tauri.

Manual release check on a clean Windows VM:

1. Open setup in English and Ukrainian at 100% and 150% display scale.
   Check headings/body text fit, keyboard navigation and contrast.
2. Install to a path containing spaces and non-ASCII characters; check Start
   menu and optional desktop shortcut. Decline launch/shortcut once as well.
3. Start ARN IDE without developer tools on PATH; open a project and confirm
   the server starts. Test missing WebView2 with internet and offline.
4. Re-run setup for same-version maintenance, then test a later version.
5. Uninstall through Windows Settings. Existing project files must remain.

Automated payload tests exercise real process loading with a Windows-only PATH,
backend version, JSONL readiness and EOF in an unrelated empty project.
They do not replace the interactive/clean-VM checks above.
