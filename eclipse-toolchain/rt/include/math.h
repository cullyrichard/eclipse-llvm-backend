#ifndef _ECLIPSE_MATH_H
#define _ECLIPSE_MATH_H

/* float only -- this target has no double (see eclipse_rt.c's soft-
 * float section header comment), so there is no plain fabs/floor/
 * ceil/sqrt taking or returning double the way real math.h does.
 * Every function here is the explicit "f" (float) variant instead,
 * matching how stdio.h's print_float sidesteps the same problem for
 * printf's own %f.
 */

float fabsf(float f);
float floorf(float f);
float ceilf(float f);

/* sqrtf is a macro, not a real function name, on purpose: llc's own
 * middle-end recognizes any function literally named "sqrtf" (by
 * name alone, matching libm's signature) and rewrites a call to it
 * into a raw FSQRT node before this project's own implementation is
 * ever reached -- this backend has no libcall registered for that
 * node, so it crashes outright ("LLVM ERROR: unsupported library
 * call operation"). Confirmed the hard way; see eclipse_rt.c's
 * sf_sqrt for the full story. Routing every call through this macro
 * to the differently-named real implementation sidesteps the
 * recognition entirely -- future math.h functions sharing a name
 * with a real libm function will likely need the same treatment.
 */
float sf_sqrt(float f);
#define sqrtf(x) sf_sqrt(x)

#endif
