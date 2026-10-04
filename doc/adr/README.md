# Architecture decision records

This folder holds the architecture decision records (ADRs) for **ethereal-samples** as used in the ethereal tree
(branch `framegraph`).

An ADR records one significant design decision: the problem, the choice, what it costs, and what else
was considered. ADRs are kept even after the decision is replaced, so that a later reader can see why
the code looks the way it does. Implementation plans and their execution logs live in
[`../plans`](../plans/README.md). An ADR states *what* was decided and *why*; a plan states *how* the
work was carried out.

Each repository of the aggregate numbers its ADRs from `0001` independently; cross-repository
references are by path. Decisions that span repositories, or that change a shared convention, belong in
the aggregate's `docs/adr` instead.

## When to write one

Write an ADR when a change:

- changes how a sample is structured in a way other samples should follow;
- adds a shared helper or convention used across samples;
- picks one of several reasonable designs and the reason is not obvious from the code.

Do not write one for local refactors, bug fixes or formatting.

## Numbering and file names

- Files are named `NNNN-<kebab-case-title>.md`, with a four-digit number that is never reused.
- Take the next free number. Numbers are assigned in order of writing, not of importance.
- Directory names are `snake_case` per the aggregate's `docs/conventions/naming.md`; the files in
  this folder are `kebab-case`, one consistent style per directory.

## Status

Each ADR has exactly one status:

| Status | Meaning |
| --- | --- |
| Proposed | Under discussion. The code may not follow it yet. |
| Accepted | In force. The code follows it. |
| Superseded by NNNN | Replaced by ADR NNNN. The text is kept unchanged except for this line. |

An accepted ADR is not rewritten when the decision changes. Write a new ADR, set the old one to
"Superseded by NNNN", and link back from the new one. Corrections of fact (a wrong path, a wrong hash)
may be made in place.

## Template

The format follows Michael Nygard's ADR format, with explicit sections for alternatives and references.

```markdown
# NNNN. Title in the imperative or as a noun phrase

- Status: Proposed | Accepted | Superseded by NNNN
- Date: YYYY-MM-DD

## Context

The forces at play: the problem, constraints, and relevant facts about the code.

## Decision

What was decided, stated so that a reviewer can check code against it.

## Consequences

### Positive
### Negative
### Risks

## Alternatives considered

Each alternative and why it was rejected.

## References

Commits (with repository), files, related ADRs and plans.
```

## Index

| ADR | Title | Status | Date |
| --- | --- | --- | --- |
| _(none yet)_ | | | |
