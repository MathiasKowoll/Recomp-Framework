// Tables emitted by the translator (gen/tables.c).
#pragma once
#include "cpu.h"
typedef struct { uint64_t addr; gfn fn; } GFunc;
typedef struct { uint64_t slot; const char *name; gfn fn; } GImport;   // IAT slot -> "dll!name"
typedef struct { const char *name; uint64_t va; uint32_t vsize, fo, fsize, flags; } GSection;
extern const GFunc gfuncs[];       extern const int ngfuncs;
extern const GImport gimports[];   extern const int ngimports;
extern const GSection gsections[]; extern const int ngsections;
extern const uint64_t gimage_base, gimage_size, gentry;
extern const uint64_t gtls_start, gtls_end, gtls_index;
void unimpl_import(CPU *c, const char *name);

// A translated DLL, loaded on LoadLibrary at its preferred base (rt.c).
typedef struct {
    const char *name;   // lower-case base name, e.g. "bink2w64"
    const GFunc *funcs; int nfuncs;
    const GImport *imports; int nimports;
    const GSection *sections; int nsections;
    uint64_t base, size, entry;
    const uint64_t *relocs; int nrelocs; int64_t delta;   // translated at a base other than the preferred one
} GModule;
void rt_register_module(const GModule *m);
void rt_module_call(CPU *c, const GModule *m, uint64_t addr);   // forwarder for an export imported by another module
