# Contributing

This repository is public and stays clean: **original tooling and method only, never
anything derived from the game** — no game code, no assets, no files extracted from a
copy of the game. Everything below exists to keep it that way while making useful
contributions easy. Contact happens through GitHub issues and pull requests on this
repository; there is no other channel.

Before anything else, read [LEGAL.md](LEGAL.md) (what may be published, what never
will) and [AGENTS.md](AGENTS.md) (the working rules and the scope of the project; the
scope boundary is at [AGENTS.md#scope](AGENTS.md#scope)).

## Reporting a bug

1. Run the report builder on your machine, from the root of your clone:

   ```bash
   python3 scripts/rapport-bug.py --journal <a log file> --lignes 200
   ```

   `--journal` can be repeated, one file per log; `--sortie FILE` writes the report to
   a file instead of stdout. It prints a Markdown report: OS and architecture, Python,
   versions of `cmake` / `cc` / `clang` / `git`, the repository revision and whether the
   tree is modified, the `SC2_*` environment variables that are set, and the tail of
   each log. Every line is scrubbed: home paths become `~/`, login `<user>`, IPv4
   `<ip>`, e-mail `<email>`, adb device serials `<serial>`, tokens and long hex strings
   `<redacted>`. It refuses (exit code 2) a log with a game-binary or asset extension,
   larger than 20 MB, or containing a NUL byte — the same limits as the CI scan.
   It never uploads anything and reads nothing but the files you give it.

2. **Read every log before attaching it.** The scrubber handles the mechanical
   patterns (paths, addresses, tokens); only you can spot what should not travel at
   all. A log that names your machine, your workplace or anything you would not post
   in public does not get attached, even scrubbed.

3. **Never attach game files.** No executable, archive, asset, save, map, screenshot
   of game content or dump taken from the game — the issue forms ask you to confirm
   this. Reports that carry such a file are closed and the file removed.

4. Open the issue with the bug report form (`.github/ISSUE_TEMPLATE/bug_report.yml`)
   and paste the output of step 1 where it asks for it.

## Pull requests

One change per pull request, and before opening it:

- [ ] `bash scripts/verifier.sh` is green end to end (the gate: host tests, Python
      unit tests, `bash -n`, fixtures — see the header of `scripts/verifier.sh`);
- [ ] CI `legal-scan` is green — it fails on game binaries and asset extensions,
      files above 20 MB, known game hashes, images outside the curated set and
      obvious secrets (`.github/workflows/legal-scan.yml`);
- [ ] every file I add is **original work or MIT-compatible tooling**, and I confirm
      it in the PR, per [LEGAL.md](LEGAL.md#enforcement--application), Enforcement point 4;
- [ ] nothing in the change describes or reaches beyond the scope defined in
      [AGENTS.md#scope](AGENTS.md#scope).

The pull-request template (`.github/pull_request_template.md`) carries this same
checklist; filling it is part of the review.

### Where derived data lives

On your machine, nowhere else. Everything the tools derive from your own copy of the
game — the lifted code, shader dumps and translated SPIR-V packs, caches, built
binaries, captures — is produced locally and stays local: `.gitignore` keeps
`extraction/`, `recomp-sc2/`, `shaders-dumps/`, `build*/`, `*.app/` and the game
archive patterns out of the repository, and the CI scan blocks them if they slip
through. A pull request that needs a file produced from the game is a pull request
that must not exist: rework it so the tool that builds the file locally is what gets
published instead. See [LEGAL.md](LEGAL.md).

### Documentation rules

- [AGENTS.md](AGENTS.md) stays under 1500 words; each guide under `docs/agents/`
  stays under 6000 words. `python3 -m unittest scripts/test_docs.py` enforces both,
  along with the rules below, and runs in CI (`.github/workflows/docs.yml`).
- Every relative Markdown link in a tracked `*.md` file must point to a file that
  exists in the repository; broken links fail the same test.
- A new guide in `docs/agents/` must be linked from `docs/agents/README.md`, the
  guide index.
- The guides live in [docs/agents/](docs/agents/): setup, runtime, rendering, the
  lift and oracle work, the hosts, benches and regression, and the symptom index.
  Write for both an autonomous coding agent and a human developer: plain English,
  tables and short numbered steps, every command grounded in a script that exists
  here.
- Source documents in French under [docs/](docs/) are translated, not copied, when
  you reuse their facts.

## Licence

Contributions are accepted under the repository licence ([LICENSE](LICENSE), MIT).
Confirming the checklist above is the confirmation that [LEGAL.md](LEGAL.md) (Enforcement, point 4) asks for.
