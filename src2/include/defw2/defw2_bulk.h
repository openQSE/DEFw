/*
 * Bulk results: what a caller lends for one, and how one is described.
 *
 * Large values never travel inside a request or a response. A caller that
 * expects one, such as a statevector, registers a buffer of its own and lends
 * it with the request. The service pushes the result into it and the
 * response carries only a tensor descriptor: element type, shape and length.
 *
 *	defw2_result_buffer_t result = {
 *		.data = malloc(nbytes),
 *		.capacity = nbytes,
 *	};
 *
 * The descriptor carries the shape so that a caller can check nbytes against
 * it before trusting either, and so that a binding can hand back an array of
 * the right type without copying. defw2_tensor_valid is that check.
 */
#ifndef DEFW2_BULK_H
#define DEFW2_BULK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Element types. Wire visible, so values may be appended, never renumbered. */
typedef enum {
	DEFW2_DTYPE_NONE	= 0,
	DEFW2_DTYPE_U8		= 1,
	DEFW2_DTYPE_I32		= 2,
	DEFW2_DTYPE_I64		= 3,
	DEFW2_DTYPE_F32		= 4,
	DEFW2_DTYPE_F64		= 5,
	DEFW2_DTYPE_C64		= 6,	/* two f32, real then imaginary */
	DEFW2_DTYPE_C128	= 7,	/* two f64, real then imaginary */
} defw2_dtype_t;

/* The most dimensions a descriptor carries. */
#define DEFW2_TENSOR_RANK_MAX	8

/*
 * What a bulk result is. Elements are in the sender's native byte order,
 * row-major. A descriptor with dtype DEFW2_DTYPE_NONE describes nothing.
 */
typedef struct {
	uint32_t	dtype;
	uint32_t	rank;
	uint64_t	shape[DEFW2_TENSOR_RANK_MAX];
	uint64_t	nbytes;
} defw2_tensor_t;

/* The size of one element, or 0 for DEFW2_DTYPE_NONE and unknown types. */
size_t defw2_dtype_size(uint32_t dtype);

/*
 * True when the descriptor is internally consistent: a known type, a rank
 * within bounds, and nbytes equal to the product of the shape times the
 * element size, with no overflow along the way.
 */
bool defw2_tensor_valid(const defw2_tensor_t *tensor);

/*
 * A one-dimensional descriptor of count elements of dtype, which is the
 * shape a statevector has. Fails when the length would overflow.
 */
bool defw2_tensor_vector(defw2_tensor_t *tensor, uint32_t dtype,
			 uint64_t count);

/*
 * A buffer the caller lends for a result. It belongs to the caller before,
 * during and after the call, and it is registered for the length of the call
 * only. capacity 0, or a NULL data, lends nothing.
 */
typedef struct {
	void		*data;
	size_t		capacity;
} defw2_result_buffer_t;

#ifdef __cplusplus
}
#endif

#endif /* DEFW2_BULK_H */
