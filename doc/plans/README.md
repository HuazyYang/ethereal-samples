# Implementation plans

This folder holds implementation plans for **ethereal-samples** work in the ethereal tree (branch `framegraph`), kept as
records after the work is done.

A plan describes *how* a piece of work is carried out: the steps, their order, the commits, and how the
result is verified. The design decisions behind the work are recorded separately as architecture
decision records in [`../adr`](../adr/README.md). A plan links to the ADRs it implements, and each ADR
links back to its plan.

A plan for work spanning several repositories belongs in the aggregate's `docs/plans` instead.

## When to write one

Write a plan for work that spans several commits, or that needs a reviewed order of steps so that every
commit still builds.

## File names

- `YYYY-MM-DD-<slug>.md`, where the date is the day the plan was approved and `<slug>` is a short
  kebab-case description, for example `2026-10-04-framegraph-samples.md`.
- Directory names are `snake_case` per the aggregate's `docs/conventions/naming.md`; the files in
  this folder are `kebab-case`, one consistent style per directory.
- Do not rename a plan when its status changes.

## Status

The first lines of each plan give its status and dates:

| Status | Meaning |
| --- | --- |
| Draft | Being written or reviewed. Not yet agreed. |
| Approved | Agreed. Work may be in progress. |
| Done | All steps finished. The plan is a historical record. |
| Abandoned | Stopped. Say why in the plan. |

## Structure

A plan should contain:

1. **Context:** the problem and the decisions already taken, with links to the ADRs.
2. **Steps:** what each step changes, in which repository, in what order.
3. **Verification:** builds, tests and runs that show the result works.

When the work is done, add:

4. **Execution record:** per step, what was done, the commits (with repository and short hash), and
   every deviation from the plan with its reason. Where the code differs from the plan, the code wins:
   do not edit the plan text to match the code; record the deviation.
5. **Verification results:** what was actually run and what passed or failed.
6. **Known issues and follow-ups:** failures found along the way, whether they were caused by the work,
   and open tasks.

Keep local user paths, machine names and credentials out of plans. Use repository-relative paths.

## Index

| Plan | Status | ADRs |
| --- | --- | --- |
| [2026-10-04-nv-asteroids-migration.md](2026-10-04-nv-asteroids-migration.md) | Approved | _(pending)_ |
