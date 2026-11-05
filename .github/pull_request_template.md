## Summary

<!-- One change per pull request: what this PR does, and why. -->

## How it was verified

<!--
How you checked the change yourself: the commands you ran and what came out,
e.g. `bash scripts/verifier.sh` (end to end), the unit tests of the script you
touched, the bench or fixture that exercises it. Say what you could NOT verify.
-->

## Checklist

<!-- The rules behind each box: CONTRIBUTING.md and LEGAL.md. -->

- [ ] One change per pull request — this PR does one thing
- [ ] `bash scripts/verifier.sh` is green end to end
- [ ] CI `legal-scan` is green (no game binaries or asset extensions, nothing above
      20 MB, no game-hash literals, no images outside the curated set, no secrets)
- [ ] Every file in this PR is original work or MIT-compatible tooling, and I confirm
      it (LEGAL.md, Enforcement point 4 — the confirmation it asks from contributors)
- [ ] Nothing here describes or reaches beyond the scope defined in
      [AGENTS.md](../AGENTS.md#scope)
- [ ] Documentation follows the rules: word limits (AGENTS.md ≤ 1500 words,
      docs/agents/*.md ≤ 6000), relative links resolve, new guides indexed —
      `python3 -m unittest scripts/test_docs.py`

## Derived data

<!-- If this PR needs a file produced from the game (lifted code, shader dumps,
     translated SPIR-V, captures): stop, it cannot be published — publish the tool
     that builds it locally instead. CONTRIBUTING.md, "Where derived data lives". -->

- [ ] Nothing in this PR derives from the game
