#pragma once
// SPDX-License-Identifier: Apache-2.0
// The unified OpenMine object model. Entities are plain value types owned by
// openmine::Project and changed only through commands (undo/redo + audit).
// Every persistent entity carries a Uuid that never changes for its lifetime.

#include "openmine/objects/CadEntities.h"
#include "openmine/objects/DesignEntities.h"
#include "openmine/objects/EnumNames.h"
#include "openmine/objects/GeologyEntities.h"
#include "openmine/objects/ProjectEntities.h"
#include "openmine/objects/ScheduleEntities.h"
#include "openmine/objects/SurveyEntities.h"
