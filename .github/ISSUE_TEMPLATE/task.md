---
name: Task (for an agent)
about: A self-contained piece of work with everything needed to do it
labels: agent-ok
---

## Context
Why this is needed; what exists already (files, functions, docs to read).

## Steps
1. …

## Acceptance
- What must be true when done (tests that pass, behaviour, docs updated).
- Tests to run: `uv run pytest`; `ctest --preset host`; firmware builds (AGENTS.md).

## Do not
- Hardware, radio, CCFG, ground_truth: see AGENTS.md "Hard rules". Anything specific to this task.
