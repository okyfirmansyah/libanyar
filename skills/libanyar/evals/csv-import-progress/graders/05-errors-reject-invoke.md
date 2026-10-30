---
type: llm
focus: trace
weight: 1
---

Failures (unreadable file, malformed CSV) are reported by throwing from the C++ handler (or replying with an error) so the frontend's invoke() rejects, and the UI handles that rejection.
