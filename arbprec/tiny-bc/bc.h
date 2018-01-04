#ifndef BC_H
#define BC_H

#include <stdint.h>

typedef enum BcStatus {

	BC_STATUS_SUCCESS,

	BC_STATUS_MALLOC_FAIL,
	BC_STATUS_INVALID_PARAM,

	BC_STATUS_LEX_INVALID_TOKEN,
	BC_STATUS_LEX_NO_STRING_END,
	BC_STATUS_LEX_NO_COMMENT_END,

} BcStatus;

#endif // BC_H
