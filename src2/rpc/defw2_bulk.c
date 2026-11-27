/*
 * Tensor descriptors. See defw2_bulk.h.
 */
#include <string.h>

#include <defw2/defw2_bulk.h>

size_t defw2_dtype_size(uint32_t dtype)
{
	switch (dtype) {
	case DEFW2_DTYPE_U8:
		return 1;
	case DEFW2_DTYPE_I32:
	case DEFW2_DTYPE_F32:
		return 4;
	case DEFW2_DTYPE_I64:
	case DEFW2_DTYPE_F64:
	case DEFW2_DTYPE_C64:
		return 8;
	case DEFW2_DTYPE_C128:
		return 16;
	default:
		return 0;
	}
}

bool defw2_tensor_valid(const defw2_tensor_t *tensor)
{
	uint64_t total;
	size_t element;
	uint32_t i;

	if (tensor == NULL)
		return false;
	element = defw2_dtype_size(tensor->dtype);
	if (element == 0 || tensor->rank == 0 ||
	    tensor->rank > DEFW2_TENSOR_RANK_MAX)
		return false;

	total = element;
	for (i = 0; i < tensor->rank; i++) {
		/* A product that wraps would make any length look right. */
		if (__builtin_mul_overflow(total, tensor->shape[i], &total))
			return false;
	}
	return total == tensor->nbytes;
}

bool defw2_tensor_vector(defw2_tensor_t *tensor, uint32_t dtype,
			 uint64_t count)
{
	size_t element = defw2_dtype_size(dtype);
	uint64_t nbytes;

	if (tensor == NULL)
		return false;
	memset(tensor, 0, sizeof(*tensor));
	if (element == 0 || __builtin_mul_overflow(count, element, &nbytes))
		return false;
	tensor->dtype = dtype;
	tensor->rank = 1;
	tensor->shape[0] = count;
	tensor->nbytes = nbytes;
	return true;
}
