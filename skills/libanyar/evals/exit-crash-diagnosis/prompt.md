---
tags: [libanyar]
max_turns: 30
timeout_seconds: 900
allowed_tools: [Read, Glob, Grep, Skill, Write, Edit, Bash]
---

Our LibAnyar app crashes maybe 1 in 10 times when the user closes the window, with "double free or corruption (fasttop)" or "malloc_consolidate(): unaligned fastbin chunk detected". It seems worse when the dashboard is busy. Here's the plugin that I suspect (files/telemetry_plugin.cpp). Find the cause, fix it, and tell me how to verify the fix since I can't reproduce it reliably by hand.

Input files: telemetry_plugin.cpp
