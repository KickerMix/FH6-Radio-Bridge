# GitHub publish checklist

Before pushing a public repository:

1. Make sure no generated artifacts are present:
   - `artifacts/`
   - `hook/build/`
   - `bin/`
   - `obj/`
   - `*.zip`
   - `*.dll`
   - `*.exe`
   - `*.pdb`
2. Check that no local game files or copied third-party binaries were added.
3. Check that no generated FH6 install folder was added, for example `fh6-radio-bridge/`.
4. Choose a project license before making the repository public. No license file is included by default.
5. Build a release locally with:

   ```powershell
   .\scripts\package_release.ps1
   ```

6. Upload only the source repository to GitHub. Upload generated release ZIPs separately under GitHub Releases, not into the repository.
