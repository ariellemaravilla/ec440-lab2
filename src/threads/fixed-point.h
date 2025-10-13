#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

/* Fixed-point real arithmetic for 17.14 format.
 * 
 * In this format:
 * - 17 bits before decimal point
 * - 14 bits after decimal point
 * - 1 sign bit
 * 
 * f = 2^14 = 16384
 */

#define FP_SHIFT 14                     /* Number of fractional bits */
#define FP_F (1 << FP_SHIFT)            /* f = 2^14 = 16384 */

/* Convert integer n to fixed point */
#define INT_TO_FP(n) ((n) * FP_F)

/* Convert fixed point x to integer (rounding toward zero) */
#define FP_TO_INT_ZERO(x) ((x) / FP_F)

/* Convert fixed point x to integer (rounding to nearest) */
#define FP_TO_INT_NEAREST(x) ((x) >= 0 ? ((x) + FP_F / 2) / FP_F \
                                       : ((x) - FP_F / 2) / FP_F)

/* Add two fixed point values */
#define FP_ADD(x, y) ((x) + (y))

/* Subtract y from x (both fixed point) */
#define FP_SUB(x, y) ((x) - (y))

/* Add fixed point x and integer n */
#define FP_ADD_INT(x, n) ((x) + (n) * FP_F)

/* Subtract integer n from fixed point x */
#define FP_SUB_INT(x, n) ((x) - (n) * FP_F)

/* Multiply two fixed point values */
#define FP_MULT(x, y) ((int)(((int64_t)(x)) * (y) / FP_F))

/* Multiply fixed point x by integer n */
#define FP_MULT_INT(x, n) ((x) * (n))

/* Divide fixed point x by fixed point y */
#define FP_DIV(x, y) ((int)(((int64_t)(x)) * FP_F / (y)))

/* Divide fixed point x by integer n */
#define FP_DIV_INT(x, n) ((x) / (n))

#endif /* threads/fixed-point.h */
