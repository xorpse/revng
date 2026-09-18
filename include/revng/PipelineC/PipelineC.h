#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#if defined(__cplusplus) && !defined(REVNG_PIPELINEC_C_ONLY)
#include <cstdint>

#include "revng/PipeboxCommon/Model.h"
#include "revng/PipeboxCommon/ObjectID.h"
#include "revng/TupleTree/TupleTreeDiff.h"
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#if defined(__cplusplus) && !defined(REVNG_PIPELINEC_C_ONLY)
#include "revng/PipelineC/ForwardDeclarations.h"
#else
#include "revng/PipelineC/ForwardDeclarationsC.h"
#endif

#include "revng/PipelineC/Callbacks.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "revng/PipelineC/Prototypes.h"

#ifdef __cplusplus
} // extern C
#endif
