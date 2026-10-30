---
type: llm
focus: trace
weight: 1
---

The answer identifies that the polling loop started with service->execute() is never stopped and joined, and replaces it with an anyar::BackgroundTask (or adds a stop flag plus join) stopped in shutdown().
