# Domain Docs

How the engineering skills should consume this repo's domain documentation when exploring the codebase.

## Before exploring, read these

- **`GLOSSARY.md`** at the repo root, or
- **`GLOSSARY-MAP.md`** at the repo root if it exists: it points at one `GLOSSARY.md` per context. Read each one relevant to the topic.
- **`docs/adr/`**: read ADRs that touch the area you're about to work in. In multi-context repos, also check `src/<context>/docs/adr/` for context-scoped decisions.

If any of these files don't exist, **proceed silently**. Don't flag their absence; don't suggest creating them upfront. The `/domain-modeling` skill (reached via `/grill-with-docs` and `/improve-codebase-architecture`) creates them lazily when terms or decisions actually get resolved.

## File structure

Single-context repo (this repo's layout):

```
/
├── GLOSSARY.md                     ← not yet created; /domain-modeling creates it lazily
├── docs/adr/                       ← 10 ADRs (0001–0009), all currently **Proposed**
├── docs/project-facts.md           ← verified engineering facts (TTA-owned, from deep analysis)
└── src/
```

## Use the glossary's vocabulary

When your output names a domain concept (in an issue title, a refactor proposal, a hypothesis, a test name), use the term as defined in `GLOSSARY.md`. Don't drift to synonyms the glossary explicitly avoids.

If the concept you need isn't in the glossary yet, that's a signal: either you're inventing language the project doesn't use (reconsider) or there's a real gap (note it for `/domain-modeling`).

Until a glossary exists, the codebase's own vocabulary in `ARCHITECTURE.md` and `docs/adr/` is the reference (e.g. Render Owner, Background Sensor, Differential Bloom, FX-only, WGC, DisplaySession, Host/Control Center).

## Flag ADR conflicts

If your output contradicts an existing ADR, surface it explicitly rather than silently overriding:

> _Contradicts ADR-0007 (background temporal validity), but worth reopening because…_

Remember: all ADRs are Proposed, so "contradicts an ADR" here means contradicting a recorded design intent — still surface it, and note the ADR's Proposed status when weighing it.
