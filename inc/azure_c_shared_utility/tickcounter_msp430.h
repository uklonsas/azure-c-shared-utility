// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifndef TICKCOUNTER_MSP430_H
#define TICKCOUNTER_MSP430_H

#include "azure_c_shared_utility/tickcounter.h"
#include "azure_c_shared_utility/umock_c_prod.h"

MOCKABLE_FUNCTION(, void, timer_a3_deinit);
MOCKABLE_FUNCTION(, void, timer_a3_init);

#endif // TICKCOUNTER_MSP430_H
