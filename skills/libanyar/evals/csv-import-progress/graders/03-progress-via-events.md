---
type: llm
focus: trace
weight: 1
---

Progress is pushed to the UI with events (app.emit / events.emit), and the frontend subscribes with listen() and unsubscribes when done.
