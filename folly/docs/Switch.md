`folly/lang/Switch.h`
---------------------

# `Switch.h`

There are 3 diagnostics for `switch` statements in C languages:
`switch-enum`, `switch-default`, and `covered-switch-default`. As with all
warnings, they may be enabled via the command line by passing `-Wswitch-enum`,
`-Wswitch-default`, `-Wcovered-switch`. Most codebases have these diagnostics
disabled, but we want to enable them in the appropriate combinations for our
codebases.

This `README` goes into detail, but as a brief overview, we want to enable
*exhaustive* `switch` statements by default, and afford having *non-exhaustive*
`switch` statements.  And we would like to do this such that the switch
statements are always compiled with the appropriate diagnostics (providing true
exhaustiveness, or flexibility if needed), regardless of the user's compiler
flags.

**Exhaustive**:
  - `switch-enum`: error
  - `switch-default`: error
  - `covered-switch-default`: ignored

User code can explicitly opt-in to exhaustive switch behavior with
`FOLLY_EXHAUSTIVE_SWITCH()`.

**Non-exhaustive**:
  - `switch-enum`: ignored
  - `switch-default`: error
  - `covered-switch-default`: error

User code can explicitly opt-in to non-exhaustive switch behavior with
`FOLLY_NON_EXHAUSTIVE_SWITCH()`.

Examples:
```
enum E {
  ONE,
  TWO,
  THREE
};

void exhaustive(E e) {
  // Failure to exhaust all enumerators and a default case will result in a
  // compiler error.
  //
  // This is true regardless of the user's compiler settings.
  FOLLY_EXHAUSTIVE_SWITCH(switch(e) {
    case E::ONE:
      // handle case
    case E::TWO:
      // handle case
    case E::THREE:
      // handle case
    default:
      // handle default case
  })
}

void non_exhaustive(E e) {
  // The compiler will not force you to exhaust all cases regardless of the
  // user's compiler settings. Accidentally exhausting all cases will result in
  // an error, as it's now an exhaustive switch.
  FOLLY_NON_EXHAUSTIVE_SWITCH(switch(e) {
    case E::ONE:
      // handle case
    default:
      // handle default case
  })
}
```

## Writing `switch` statements in a C++ codebase

C++ permits casting any arbitrary integer value to any `enum class` instance as
if we were casting to an integer of the underlying type of the `enum class`, so
they are opaque integers. `enum`s can be converted to-and-from any integer
implicitly, so they are just transluscent integers. Notably, this is regardless
of whether the value is an enumerator. So a truly exhaustive switch-case
statement must cover all possible values of an enum variable, including the case
when the value is not equivalent to any of the enum's enumerators (see
[`std::byte`](https://en.cppreference.com/w/cpp/types/byte.html) for an example
of an enum without any enumerators!). Unenumerated values also enable patterns
like the following, where the enumerators are themselves exceptional:

```
enum class logposition_t : std::int64_t {
  sealed = -1,
  nonexistent = -2,
};

void handle(logposition_t logpos) {
  switch (logpos) {
    case losposition_t::sealed:
      // handle sealed case
    case logposition_t::nonexistent:
      // handle nonexistent case
    default:
      // handle non-exceptional case
  }
}
```

Though there are innumerable ways to write a `switch` statement, there are two
recommended ways to write a `switch` statement: exhaustive switches and
non-exhaustive switches (all others are discouraged).  Exhaustive switches
should be the default go-to pattern for switch statements.

The basis of what cases can be listed in a `switch` statement is the type of
the value being switched on. If the value being switched on is an enum, then
the cases can be its enumerators, and since enumerators are finite, that
`switch` can have a case for every singled defined enumerator. i.e. a
`switch` statement can *exhaust* all possible values of the switched enum
variable. If the value being switched on is not an enum (i.e. it's an integer
type), then the cases can be any value of the integer type, and though integer
types are not infinite, they are expansive enough that it is impractical
`switch` statement cannot reasonably have a case for every possible value of
the switched integer variable; i.e.  a `switch` statement must be
*non-exhaustive* and only have cases for specific values of the expansive
possibilities.

When compiling, if `switch` argument is not an enum, it will *always* be a
*non-exhaustive* statement (since there's no enum to exhaust). The `switch-enum`
diagnostic and the `covered-switch-default` diagnostic will be ignored. The
`switch-default` diagnostic will still be enforced though (which is good).

For a `switch` statement that handles all enumerators of en enum. Not handling
the default value often leads to unexpected (or even undefined) behavior. This
has caused many, *many* SEVs for us. For example,

```
enum E {
  ONE,
  TWO,
};

std::unordered_map<std::int64_t, std::string> data_;

void cacheData(std::int64_t key, E e) {
  if (!isKeyValid(key)) {
    return;
  }

  switch (e) {
    case E::ONE:
      data_[key] = foo(key, e);
      break;
    case E::TWO:
      data_[key] = bar(key, e);
      break;
  }

  LOG(INFO) << "Stored value " << data_[key];
}
```

So, reiterating cogently:

  * `switch` statements on non-enum values are always *non-exhaustive* and always need a `default` case
  * `switch` statements on enum values must always handle non-enumerator values with a `default` case
  * `switch` statements on enum values can be *exhaustive* or *non-exhaustive*, and we should default to *exhaustive*
  * `switch` statements without a `default` case risk SEVs from **unexpected behavior** (and is why *Clang 18* has implemented `switch-default`)
  * We want to have the compiler help us keep our `switch` statements explicit and safe. Regardless of the compiler flags used by the user's build.

## The compiler diagnostics for `switch` statements

- **`switch-enum`**
Require all defined enum values to be listed as cases in a `switch` statement

- **`switch-default`**
Require a `default` case to be present in a `switch` statement. Added to Clang
18 (was already in GCC):
https://releases.llvm.org/18.1.6/tools/clang/docs/ReleaseNotes.html

- **`covered-switch-default`**
Warn when a `default` case is present but all enum values are listed as cases
(most useful for scoping a *non-exhaustive* `switch` statement and detecting when it
has become *exhaustive* to either convert to an *exhaustive* `switch` statement
or fix whatever expectation has been violated).

Though technically feasible, it is a bad pattern to write a `switch` statement
that is non-exhaustive for an enum (i.e. one that does not exhaust the enum).
This is because if the enum should expand in the future, the `switch` statement
can neglect these new enum values, and there will be nothing to indicate the
need for the `switch` statement to be updated.  To combat this, we can use
`switch-enum` to enforce that enums are exhausted in a `switch` statement.

Though technically feasible, it is a bad pattern to write a `switch` statement
that is meant to be non-exhaustive, but actually exhausts all possible cases. This
leads to confusion in the expectation for what the `default` is handling. It
would appear that a non-exhaustive `switch` statement with a `default` case is
covering some enumerators, but it is not actually covering any enumerators. To
combat this, we can use `covered-switch-default` to highlight that we have an
*exhaustive* switch despite being meant to be *non-exhaustive*, where we can
then convert the `switch` statement to instead be exhaustive.

Though technically feasible, it is a bad pattern to write a `switch` statement
that exhausts all possible cases, but does not have a `default` case. The
`default` is not meant for handling any of the enumerators, but rather for
handling any non-enumerator values (since `enum class` types are just opaque
integers, and `enum`s are transluscent integers, you can pass a value that is
not defined in the enum as an enum value). To combat this, we can use
`switch-default` to ensure that even exhaustive `switch` statements have a
`default` case which ensures we truly exhaust the enum and avoid unexpected (or
even undefined) behavior.

Thus... with these three compiler diagnostics, we can defined diagnostics to
scope a `switch` statement in one of the 2 legitimate ways: either *exhaustive*
or *non-exhaustive*.

**Exhaustive**:
  - `switch-enum`: error
  - `switch-default`: error
  - `covered-switch-default`: ignored

**Non-exhaustive**:
  - `switch-enum`: ignored
  - `switch-default`: error
  - `covered-switch-default`: error

Keep in mind that when you `switch` on a non-enum value, it is always
*non-exhaustive*, so only `switch-default` will have an effect.

## Exhaustive switches

When you `switch` on the enumerators of an `enum`, you write an exhaustive
switch.

For example:

```
enum class Color { Red, Green, Blue };

void print_color(Color c) {
  switch (c) {
    case Color::Red:
      std::cout << "Red";
      break;
    case Color::Green:
      std::cout << "Green";
      break;
    case Color::Blue:
      std::cout << "Blue";
      break;
    default:
      std::cout << "Unknown";
      break;
  }
}
```

With a finite set of enum values, you exhaust the enum by listing all of the
values as cases. But since an enum is nothing but a transluscent integer in
C++ (as explained above), code should handle the (potentially) exceptional case
of a non-enumerator value being passed to the switch. This is done by adding a
`default` case to the switch.

This is the default use case for `switch` statements.

For the exhaustive `switch` use case, we want to enable `switch-enum` and
`switch-default`, but disable `covered-switch-default`. For convenience,
you can use the `FOLLY_EXHAUSTIVE_SWITCH()` macro.

NOTE: having the `switch` diagnostics configured for exhaustive switches OUGHT
to be the default for all C++ codebases. However, due to the copious usage of
`switch` statements in our codebase without these improved diagnostics, it will
take time to get there. In the meantime, we can use the
`FOLLY_EXHAUSTIVE_SWITCH()` macro to explicitly opt-in to the exhaustive switch
behavior.

## Non-exhaustive switches

When you `switch` on the value of a concretely defined enum with a very large
set of values, and only need to address a sane subset of the values, you write
a non-exhaustive switch.  Additionally, when you `switch` on the a non-enum
value (like an `int`), you write a non-exhaustive switch.

For example (big enum):

```
enum class Color {
  ...
  every color in a 64 color palette
  named as an enum value
  ...
};

bool is_red_color(Color c) {
  switch (c) {
    case Color::Red:
    case Color::LightRed:
    case Color::DarkRed:
      return true;
    default:
      return false;
  }
}
```

For example (non-enum):

```
typedef char Color; // https://en.wikipedia.org/wiki/ANSI_escape_code#8-bit

bool is_red_color(Color c) {
  switch (c) {
    case 0x09: // "high intensity" red
    case 0xC4: // mid red
    case 0xC5: // light red
    case 0xA0: // dark red
      return true;
    default:
      return false;
  }
}
```

With a large set of enum values (or non-enum integer values), you do not want
to exhaust all the values as cases. Instead, you want to list the values you
care about as cases, and then use a `default` case to handle the remaining
cases with a sane default behavior.

This is the less common use case for `switch` statements.

To gain compiler support for non-exhaustive switches, we can leverage the three
`switch` behavior compiler diagnostics:

For the non-exhaustive `switch` use case, we want to `covered-switch-default`
and `switch-default`, and disable `switch-enum`.  This way, if the code does
exhaust all the enum values, the compiler will warn us so that we can flip over
to the exhaustive switch behavior.

For convenience, you can use the `FOLLY_NON_EXHAUSTIVE_SWITCH()` macro.
