# Claude Code skills for LibAnyar

| Skill | For |
|---|---|
| [`libanyar/`](libanyar/SKILL.md) | Developers **building apps with LibAnyar**: the threading model, commands/events/plugins, binary data (SharedBuffer, Pinhole), shutdown rules, debugging and UI test scripts, packaging and asset licensing. |

The skill is versioned with the framework, so its API notes match this checkout.

## Install

Pick one:

```bash
# For all your projects
mkdir -p ~/.claude/skills && cp -r skills/libanyar ~/.claude/skills/

# For one app project (commit it, so your whole team gets it)
mkdir -p my-app/.claude/skills && cp -r skills/libanyar my-app/.claude/skills/
```

Claude Code picks it up automatically when you work on LibAnyar code: it is triggered by mentions of LibAnyar, `anyar::App`, `@libanyar/api`, the `anyar` CLI and the like.

## Layout

```
libanyar/
├── SKILL.md                 # mental model, rules, workflow (always loaded when triggered)
├── references/              # read on demand
│   ├── cpp-backend.md
│   ├── frontend.md
│   ├── binary-data-and-rendering.md
│   ├── debugging-and-testing.md
│   └── build-and-ship.md
└── evals/                   # test cases (claude plugin eval format)
```

## Evaluating changes

`evals/` holds realistic tasks with graded expectations: camera streaming, a large CSV import, and an exit-crash diagnosis. With Claude Code v2.1.269+:

```bash
cd skills/libanyar && claude plugin eval . --allow-tools Write,Edit,Bash --scaffold
```

This runs every case with and without the skill and reports the pass rate. The runs are real, billed model runs.
