/*
 * Shared types for DEFw v2.
 *
 * Status categories are wire visible, so values may be appended but never
 * renumbered. The first group covers the transport and control failures any
 * caller can hit. The second covers the QPM outcomes the QFw C requirements
 * name, which v1 expressed as Python exception classes.
 */
#ifndef DEFW2_TYPES_H
#define DEFW2_TYPES_H

#include <stdint.h>

#define DEFW2_VERSION_MAJOR 0
#define DEFW2_VERSION_MINOR 1

#define DEFW2_STR(value)	#value
#define DEFW2_XSTR(value)	DEFW2_STR(value)
#define DEFW2_VERSION_STRING	DEFW2_XSTR(DEFW2_VERSION_MAJOR) "." \
				DEFW2_XSTR(DEFW2_VERSION_MINOR)

/* Return codes. Success is 0 and every failure is negative. */
typedef enum {
	DEFW2_OK		= 0,
	DEFW2_ERR_INVALID	= -1,
	DEFW2_ERR_NOMEM		= -2,
	DEFW2_ERR_CONFIG	= -3,
	DEFW2_ERR_TRANSPORT	= -4,
	DEFW2_ERR_TIMEOUT	= -5,
	DEFW2_ERR_CANCELLED	= -6,
	DEFW2_ERR_NOT_FOUND	= -7,
	DEFW2_ERR_VERSION	= -8,
	DEFW2_ERR_INTERNAL	= -9,
	DEFW2_ERR_BUSY		= -10,
} defw2_rc_t;

typedef enum {
	DEFW2_CAT_OK			= 0,
	DEFW2_CAT_TRANSPORT		= 1,
	DEFW2_CAT_TIMEOUT		= 2,
	DEFW2_CAT_CANCELLED		= 3,
	DEFW2_CAT_NOT_FOUND		= 4,
	DEFW2_CAT_VERSION_MISMATCH	= 5,
	DEFW2_CAT_INVALID_ARGUMENT	= 6,
	DEFW2_CAT_INVALID_RESERVATION	= 7,
	DEFW2_CAT_INSUFFICIENT_ALLOWANCE = 8,
	DEFW2_CAT_PENDING_CAPACITY	= 9,
	DEFW2_CAT_POLICY_DELAYED	= 10,
	DEFW2_CAT_EXPIRED_RESERVATION	= 11,
	DEFW2_CAT_SCHEDULER_FAILURE	= 12,
	DEFW2_CAT_PROVIDER_FAILURE	= 13,
} defw2_category_t;

/*
 * Every RPC returns one of these before any payload. message is owned by
 * the receiving side and freed with defw2_status_free.
 */
typedef struct {
	int32_t		code;
	uint32_t	category;
	char		*message;
} defw2_status_t;

#endif /* DEFW2_TYPES_H */
