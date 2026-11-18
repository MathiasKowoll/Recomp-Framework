// Tables the win32 translator writes (gen/tables.c).
#pragma once
#include "cpu32.h"
typedef struct { uint32_t addr; gfn fn; } GFunc;
typedef struct { uint32_t slot; const char *name; gfn fn; } GImport;   // IAT slot, "dll!name", shim
typedef struct { const char *name; uint32_t va, vsize, foff, fsize, flags; } GSection;
extern const GFunc gfuncs[]; extern const int ngfuncs;
extern const GImport gimports[]; extern const int ngimports;
extern const GSection gsections[]; extern const int ngsections;
extern const uint32_t gimage_base, gimage_size, gentry;
