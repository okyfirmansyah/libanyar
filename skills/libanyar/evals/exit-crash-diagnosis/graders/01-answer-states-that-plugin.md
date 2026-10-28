---
type: llm
focus: trace
weight: 1
---

The answer states that plugin shutdown() runs on the main/UI thread while command handlers (e.g. telemetry:release) can still run on the service thread, so the sessions_ map is accessed concurrently without synchronization.
