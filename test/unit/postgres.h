/*
 * postgres.h stub for unit tests
 *
 * This file is found before the real postgres.h when compiling unit tests
 * (due to -I test/unit appearing first in the include path).
 * It provides just enough compatibility to include source headers directly.
 */
#pragma once

#include "pg_stubs.h"
