---
type: llm
focus: trace
weight: 1
---

The file path from the frontend is converted with anyar::path_from_utf8 (or an equivalent UTF-8-safe conversion), not with std::filesystem::path(std::string) or std::ifstream on the raw string.
