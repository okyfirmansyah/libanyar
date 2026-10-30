---
type: llm
focus: trace
weight: 1
---

The 2 GB read/parse/insert work does not run directly in the command handler on the service thread: it is wrapped in anyar::run_blocking or runs in an anyar::BackgroundTask.
