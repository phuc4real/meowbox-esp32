---
name: release-docs
description: Bring MeowBox's README files and docs in line with the code before a release - find what changed since the last release tag, check each claim against the code, fix README.md, README_vi.md, firmware/README.md, API.md, pc-agent/README.md, CLAUDE.md, hardware and roadmap docs, then report. Use whenever a new version is about to be released or tagged, before running firmware/tools/release.py, when bumping PROJECT_VER, or when the user asks to update / sync / check the docs or README, or says they are outdated, even if they don't mention a release.
---

# Release docs: make the docs say what the code does

A release ships code *and* a description of it. When the description lags, users follow steps that no longer exist
(the middle button "stops alarms", `meowbox discover`) and the next agent trusts CLAUDE.md numbers that are a
version old. This skill finds the gap between the code and the docs since the last release, closes it, and leaves
the edits uncommitted for the user to review. It does not release anything; `/flash-speaker` and
`firmware/tools/release.py` stay separate steps.

Paths are relative to the repo root.

## 1. Run the mechanical check

```bash
py -3.14 .claude/skills/release-docs/scripts/doc_check.py            # since the latest tag; --since <tag> to pick
```

It reports: the commits since the release, areas that changed while the docs describing them did not, broken
relative links, API.md vs the firmware's routes (and the mock server), vi/en string keys, PC agent commands named in
the docs that don't exist, version numbers. Treat every line as a lead, not a verdict: an untouched doc may still be
right, and an "older version" mention may be history on purpose. The check can't see meaning: a doc can be touched
in the range and still be wrong, so step 2 matters more.

## 2. Build the list of user-visible changes from the code

Read `git log --format='%h %s%n%b' <tag>..HEAD` and, for each commit that changes behaviour, the diff
(`git show --stat <h>`, then the relevant hunks). Commit messages describe intent; the docs must describe what the
code does now, so confirm each fact in the current source before writing it: a default in `settings.c`, a key in
`lang/*.json`, a GPIO in `hw/ostb_xiaozhi_3st.h`, an argparse option in `pc-agent/meowbox_agent/cli.py`, a route in
`web_register(...)`. Later commits often revert or refine earlier ones, so the final state wins.

Note numbers from the code or measurements (sizes, RAM, timeouts, limits), not from memory: `idf.py size` or the
`.bin` size for the app, `docs/perf/results-<ver>.md` for RAM.

## 3. Update the owning doc

Each fact has one home; other docs link to it. Edit the owner, then fix the places that repeat it.

| What changed | Owner | Also check |
|---|---|---|
| How to use the speaker (buttons, screens, settings, alarms, radio, notifications, agent install) | `README_vi.md` (the user guide, Vietnamese) | `firmware/README.md` "Using it" table |
| Project status, what a version brought, layout, doc index | `README.md` (status checklist: add the release line) | |
| Build, flash, components, partitions, app size, release steps | `firmware/README.md` | `CLAUDE.md` Environment |
| REST endpoints, fields, defaults, limits | `firmware/web/API.md` (the contract) | `web/mock_server.py`, `web/test_api.py`, `pc-agent/mock_speaker.py` are code: list mismatches in the report, fix only if trivial |
| Web UI behaviour | `firmware/web/README.md` | |
| PC agent commands, connect flow, build | `pc-agent/README.md` | `README_vi.md` agent section |
| A hardware fact | `hw/ostb_xiaozhi_3st.h` + `docs/pinmap.md` together | `docs/hardware.md`, `README.md` hardware table |
| Measured RAM / CPU | `docs/perf/results-<ver>.md` | `CLAUDE.md` "Internal RAM" line |
| Tooling, env, gotchas agents need | `CLAUDE.md` | `.claude/skills/*/SKILL.md` commands |
| Roadmap item done | `docs/roadmap.md` (mark it, say in which version) | |

Plans and records (`docs/firmware_v0.2.0.md`, `docs/plan.md`, `docs/ui_redesign.md`, `docs/perf/results-0.2.0.md`,
`docs/reverse-engineering.md`) are history: leave them, except a present-tense statement that is now false and
misleads (then correct it with a date, or add a one-line pointer to where the current fact lives).

Writing rules (from CLAUDE.md): English everywhere except `README_vi.md`, which is a plain Vietnamese user guide for
people who just want to use the speaker (short sentences, no internals). Never write device identifiers (MAC, real
IPs, Wi-Fi names, tokens); use `192.168.1.50`-style examples. Keep the existing tone and structure of each file;
change what is wrong, don't rewrite what is right.

## 4. Re-check

Run `doc_check.py` again: links and lists must be clean or explained. If `firmware/web/src/` changed, the web tests
still apply (`node --check src/app.js`, `py -3.14 test_api.py` in `firmware/web/`).

## 5. Report, don't commit

Leave the edits in the working tree and answer with:

```
## Docs for <version> (<tag>..HEAD, N commits)
### Updated
| File | What changed | Checked against |
| README_vi.md | alarm stop: hold VOL ~1 s (was: middle button) | firmware/main/main.c on_button() |
### Still open (needs a decision or code)
- GET /api/battery/log.csv is in API.md but not in mock_server.py
### Checked, no change needed
- docs/hardware.md: no hardware change in the range
```

Then suggest the commit message (one English sentence, imperative, user-visible, per CLAUDE.md), e.g.
`Update the README and docs for 0.2.2.` The user reviews the diff and commits; the release comes after.
