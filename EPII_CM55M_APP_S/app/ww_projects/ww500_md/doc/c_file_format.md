# C/H File Formatting Standard

This document defines the preferred layout for `.c` and `.h` files in
C projects. It is modelled on `pca9574.c` / `pca9574.h`, which are the
reference examples of the style. Use this document when creating new .c & .h files.
Use it _when requested_ to reformat files that
don't follow this arrangement (e.g. `directory_manager.c/h`)
— content/behaviour should not change, only structure and comments.

## Goals

- All elements of a given kind live together (all includes together, all
  `#define`s together, all public functions together, etc.) rather than being
  scattered through the file in the order they happened to be written.
- A reader can jump to the right section via the banner comments without
  scanning the whole file.
- Every function (public and private) has a Doxygen-style header so
  behaviour is documented next to the declaration/definition, not inferred
  from the body.

## Section banner format

Section dividers are single-line comments, asterisk-padded to a consistent
width (~100–110 columns), in this form:

```c
/*********************************************** Includes ****************************************************/
```

Keep the same banner style/width throughout a file. Leave a blank line after
a banner before the content starts, and a blank line before the next banner.
An empty section (nothing to put there yet) is left as just the banner with
a blank line under it — don't delete the banner.

## `.c` file layout, in order

1. **File header comment** (top of file, before anything else)
2. `Includes`
3. `Local Defines` (`#define`, file-local constants)
4. `Local Types` (file-local `typedef`/`struct`/`enum`) — omit banner if unused
5. `External Variables` (sometimes required as in `CLI-commands.c`)
6. `Local Variables` (file-local/static state)
7. `Local Function Declarations` (forward declarations of `static` functions)
8. `Local Function Definitions` (bodies of the `static` functions, in the
   same order as their declarations)
9. `Global Function Definitions` (bodies of the functions declared in the
   matching `.h`, in the same order as they appear in the `.h`)

## `.h` file layout, in order

1. **File header comment**
2. Include guard (`#ifndef ... #define ...`)
3. `Includes`
4. `Global Defines`
5. `Global Types` (public `typedef`/`struct`/`enum`)
6. `Global Variables` (`extern` declarations, if any)
7. `Global Function Declarations`
8. `#endif`

Wrap declarations in `extern "C" { ... }` guarded by `#ifdef __cplusplus`
when the header may be included from C++ (see `inactivity.h`).

## File header comment

At the top of every file:

```c
/*
 * <filename>
 *
 *  Created on: <date>
 *      Author: <name>
 *
 *  <one-paragraph description of what this file/module does>
 *
 *  <optional notes, caveats, TODOs>
 */
```

## Function header comment (Doxygen style)

Every function — public or `static` — gets a header immediately above it,
directly above the declaration in a `.h` and above the definition in a `.c`.
Don't duplicate the full header on both; the `.c` definition carries the
full Doxygen block, the `.h` declaration may carry a one-line `@brief` only
if useful for callers skimming the header.

```c
/**
 * @brief One-line summary of what the function does.
 *
 * Optional longer description: behaviour, side effects, assumptions,
 * what happens on error, threading/ISR considerations, etc.
 *
 * @param deviceAddr I2C address of the device to write to.
 * @param reg        Register offset to write.
 * @param val        Value to write.
 * @return HX_CIS_ERROR_E error code (HX_CIS_NO_ERROR on success).
 */
HX_CIS_ERROR_E pca9574_write(uint8_t deviceAddr, uint8_t reg, uint8_t val);
```

Rules:
- Always include `@brief`.
- Include one `@param` per parameter, in order, even if the description is
  short.
- Include `@return` unless the function is `void`.
- Note ISR-safety / task-context requirements explicitly when relevant
  (see `inactivity_reset()` for an example of why this matters).

## Grouping rules

- **Defines**: all `#define`s live in the `Local Defines` / `Global Defines`
  section — none scattered next to the code that uses them, unless a value
  is genuinely local to one function (e.g. a magic number used once), in
  which case a local `const` inside the function is preferable to a stray
  `#define`.
- **Static/private functions**: declared together in
  `Local Function Declarations`, defined together in
  `Local Function Definitions`. Never interleave a `static` helper's
  definition between two public function definitions.
- **Public functions**: defined together in `Global Function Definitions`,
  in the same order as declared in the header, so the `.c` and `.h` can be
  read side by side.
- Commented-out dead code should be removed, not carried forward — if it's
  worth keeping for reference, it belongs in git history or a doc note, not
  inline as commented-out blocks (see the messy `typedef`/`define` blocks in
  the current `directory_manager.h` as an example of what to clean up).

## Public names

This applies when creating new files only - don't rename existing public functions unless requested.

Public function names should all begin with some characters derived from teh file name
such as `pca9574_init()`. 

Public constant definitions are to follow the same pattern, as in `PCA9574_I2C_ADDRESS_0`.

## What does *not* change

- No behavioural changes: control flow, logic, variable values, and public
  API signatures stay the same.
- Existing TODO/FIXME notes are preserved (moved into the appropriate
  section/header comment, not deleted).
- Naming conventions already in use in the file are kept as-is unless
  explicitly asked to rename.
