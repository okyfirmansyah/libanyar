---
tags: [libanyar]
max_turns: 30
timeout_seconds: 900
allowed_tools: [Read, Glob, Grep, Skill, Write, Edit, Bash]
---

I'm building a LibAnyar desktop app that shows a live USB camera preview. I already have a C++ function `bool grab_frame(uint8_t* rgba_out)` that blocks for ~16 ms and fills one 1280x720 RGBA frame. Write me a LibAnyar plugin (header + cpp) that streams these frames to the frontend at camera rate with `camera:start` / `camera:stop` commands, plus the TypeScript side that draws them into a <canvas id="preview">. The app must also close cleanly if the user quits while the camera is running. Put the files in ./camera/.
