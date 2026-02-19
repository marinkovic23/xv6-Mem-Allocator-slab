#ifndef UCACHE_H
#define UCACHE_H

#include "kernel/types.h"

struct ucent {
    uint64 kc;
    void (*ctor)(void*);
    void (*dtor)(void*);
    int objsz;
    int used;
};

struct ucent* find(uint64 kc);
struct ucent* insert(uint64 kc, int objsz, void (*ctor)(void*), void (*dtor)(void*));
void remove_entry(uint64 kc);

// DEBUG helpers:
void ucache_dump_one(uint64 kc);
void ucache_dump_all(void);


#endif