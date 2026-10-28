---
tags: [libanyar]
max_turns: 30
timeout_seconds: 900
allowed_tools: [Read, Glob, Grep, Skill, Write, Edit, Bash]
---

In my LibAnyar notes app (Svelte frontend), add an "Import CSV" button. The user picks a file with a native file dialog; the CSV can be 2 GB, so parse it in C++, insert rows into my existing SQLite table `notes(title, body)` that I already open with SOCI in my `NotesPlugin`, and show a progress bar while it runs. File names can contain non-ASCII characters (we have Indonesian and Japanese users). Show me the C++ command and the Svelte/TS changes.
