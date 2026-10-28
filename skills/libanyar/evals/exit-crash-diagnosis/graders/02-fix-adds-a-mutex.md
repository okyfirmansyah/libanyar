---
type: llm
focus: trace
weight: 1
---

The fix adds a mutex (or equivalent synchronization) protecting sessions_ in shutdown() and in every command that touches it.
