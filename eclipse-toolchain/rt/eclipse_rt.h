#ifndef ECLIPSE_RT_H
#define ECLIPSE_RT_H

/* Eclipse runtime library — umbrella header.
 *
 * Historically the only way to reach this runtime library; the real
 * standard headers (stdio.h, string.h, ctype.h, stdlib.h, in
 * eclipse-toolchain/rt/include/) now declare the same functions plus
 * more. Kept as a convenience "include everything" header and for
 * existing source files that already say `#include "eclipse_rt.h"`.
 * Prefer the standard headers in new code.
 */

#include "stdio.h"
#include "string.h"
#include "ctype.h"
#include "stdlib.h"
#include "math.h"
#include "eclipse_io.h"

#endif
