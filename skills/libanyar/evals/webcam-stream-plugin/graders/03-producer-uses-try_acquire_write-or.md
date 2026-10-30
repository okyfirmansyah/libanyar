---
type: llm
focus: trace
weight: 1
---

The producer uses try_acquire_write (or otherwise drops frames) instead of a blocking acquire_write without a close path.
