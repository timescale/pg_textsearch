#include <postgres.h>

#include "constants.h"
#include "segment/compaction.h"

uint64
tp_max_segment_size_bytes(void)
{
	return (uint64)tp_max_segment_size_mb * 1024 * 1024;
}
