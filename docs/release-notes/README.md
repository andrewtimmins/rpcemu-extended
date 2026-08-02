# Release notes

One file per release, named for its tag: `v1.1.12.md` for `v1.1.12`. The file is
what GitHub publishes as the release body, so it is written for the people
downloading the build, not for the person who made it.

The tag will not build without one. `.github/workflows/build.yml` checks the file
exists and is not empty before anything is compiled, and the release job passes it
to GitHub as `body_path`.

## Why the file exists

GitHub can generate release notes on its own, and this project used to let it.
What it generates is a list of merged pull requests. Nearly all the work here
lands as direct commits to `main`, so the generated notes described a small
fraction of each release: 1.1.9 announced itself as a single unrelated pull
request, and 1.1.11 read as though a hundred commits of new features were some
changes to the build system. Both were rewritten by hand after somebody noticed.
Writing the file first makes that impossible to forget.

## What goes in one

Look at `v1.1.12.md`, which is the shape to copy.

- **Lead with what a user will notice**, not with the largest diff.
- **Group the rest**: new features, fixes, then under the hood. A reader
  deciding whether to upgrade should be able to stop after the first group.
- **Say what is verified and what is not.** "Tested on Linux only", "wants
  confirming on real hardware", "still open" - these are the lines that keep
  people trusting the rest of the notes. Never imply something has been proven
  on a platform where it has not been run.
- **Credit contributors by name** and link their pull requests.
- **Close with a compare link** to the previous release, so the full commit list
  is one click away:
  `**Full changelog**: https://github.com/<owner>/<repo>/compare/vPREV...vTHIS`

## Cutting a release

1. Write `docs/release-notes/vX.Y.Z.md`.
2. Set `VERSION` to `X.Y.Z` and commit both together.
3. Push `main` and wait for every build job to go green.
4. Tag `vX.Y.Z`, annotated, and push the tag.

The tag run checks the tag against `VERSION` and the notes file against the tag
before it builds anything, so a mistake at step 1 or 2 costs seconds rather than
a published release that has to be edited.
