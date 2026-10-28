---
type: llm
focus: trace
weight: 1
---

The TypeScript code releases each consumed frame/slot back to the C++ side (e.g. a release command) in a finally block or equivalent, and unsubscribes its listener when stopped.
