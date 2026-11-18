---
name: folly-dependencies
metadata:
  oncalls: fbcode_entropy_wardens_folly
  strict: true
  apply_to_path: '^xplat/folly/(.*/)?(BUCK|[^/]*\.(h|cpp))$'
---

# Folly dependencies

- Do not add new Boost uses; use the C++ standard library or Folly instead.
- Declare each third-party dependency in every target whose own sources include
  it, not through another target's exports. Then update CMake with
  `xplat/folly/facebook/generate_cmake.py`.
