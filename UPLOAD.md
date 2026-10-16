# Putting Felucca GEN on GitHub (web upload, no terminal)

This folder is the whole difference between stock Felucca 1.6.1 (commit a53ecf4) and the GEN
firmware: 58 files, so it fits GitHub's 100-files-per-upload limit in one go. Everything else
comes from the upstream repository, which you fork rather than upload.

1. Fork https://github.com/hugelton/Felucca on GitHub (the "Fork" button, top right).
   The fork carries the engines submodule, the toolchain scripts and the licences with it.
2. In your fork: **Add file → Upload files**. Drag the *folders* from this archive
   (`.github`, `firmware`, `tests`, `tools`, `docs`) and the loose files (`GEN.md`) onto the page.
   Dragging folders keeps the paths; files that already exist are replaced.
   (`.github` is a hidden folder on Mac: in the Finder press Cmd-Shift-. to see it, or upload
   `.github/workflows/build.yml` on its own with "create new file" and paste its contents.)
3. Commit message: `GEN: generative sequencer, RISO look, methods, forms`. Commit to `main`.
4. The Actions tab now runs `build.yml`: it fetches the JieLi toolchain and SDK, runs the host
   tests, builds `felucca-gen.fwsc`, and publishes the web installer to GitHub Pages
   (Settings → Pages → Source: GitHub Actions, once). The firmware is under the run's Artifacts.

If you would rather use git: `felucca-gen.patch` is the same change as one patch series;
`git am felucca-gen.patch` on a clone of Felucca at a53ecf4 reproduces the ten commits.
