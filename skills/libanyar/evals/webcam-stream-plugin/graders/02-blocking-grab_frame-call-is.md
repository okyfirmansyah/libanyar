---
type: llm
focus: trace
weight: 1
---

The blocking grab_frame call is never made directly on the service thread: it runs inside anyar::run_blocking (or on a dedicated worker), and the long-running loop is an anyar::BackgroundTask or another stoppable loop that is joined on stop.
