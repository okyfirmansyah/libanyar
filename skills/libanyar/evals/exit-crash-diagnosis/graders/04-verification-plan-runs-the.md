---
type: llm
focus: trace
weight: 1
---

The verification plan runs the app repeatedly and automatically, exiting at varied or random moments (e.g. an ANYAR_TEST_SCRIPT calling anyarTest.done after a random delay, or debug:exit / window:close-all), and compares the crash count before and after the fix.
