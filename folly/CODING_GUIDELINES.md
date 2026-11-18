# Folly Coding Guidelines


## The Spirit of Folly

Folly is a collection of core libraries, which engineers depend on.
Therefore, the primary requirements when writing Folly code are:

1. Create reusable components. Do one job, and do it well.
2. Be complete. Missing APIs create friction for users.
3. Have strong unit tests. Uphold Folly's reputation of reliability.
4. Have documentation. Not everyone's an expert.


## The realities of Folly

As an open-source library, there are a few necessary requirements:

1. Define symbols in namespace `folly`. Prefix macros with `FOLLY_`, and C symbols with `folly_`.
2. Put library-private symbols in subnamespace `detail` (e.g. `folly::detail::XYZ`)
3. Restrict dependencies to (a) the standard library, and (b) other Folly libraries.
   Do not add new Boost uses, and declare each third-party dependency in every target whose own
   sources include it.
4. Be portable. Notably:
   - Use `throw_exception` from `folly/lang/Exception.h` instead of `throw`


Folly only reimplements existing standard or boost libraries when there is good reason to.
When improving upon an already-existing library:

1. Strive to be a drop-in replacement:
   - Have the same APIs
   - Have the same guarantees
   - Have the same naming convention


## Coding style

Folly follows Meta's C++ Coding conventions. When changing a file that has non-standard
style, consistency takes priority.

Some Folly files, based on standard classes, use naming conventions from the C++ standard:
`snake_case` instead of `camelCase`. If you are faced with this situation, use your judgement to
decide which casing to use.


## Documentation style

Follow the [Google developer documentation style guide](https://developers.google.com/style). Highlights:

- Use standard American spelling and punctuation.
- Use second person: "you" rather than "we."
- Use active voice: make clear who's performing the action.
- Be conversational and friendly.
