#include "ucache.h"
#include "user.h"


#define MAX_UCACHES 128


struct ucent tab[MAX_UCACHES];

// linear search is fine for small MAX_UCACHES
struct ucent* find(uint64 kc) {
    for(int i=0;i<MAX_UCACHES;i++)
        if(tab[i].used && tab[i].kc == kc) return &tab[i];
    return 0;
}

struct ucent* insert(uint64 kc, int objsz, void (*ctor)(void*), void (*dtor)(void*)) {
    for(int i=0;i<MAX_UCACHES;i++){
        if(!tab[i].used){
            tab[i].used = 1;
            tab[i].kc = kc;
            tab[i].ctor = ctor;
            tab[i].dtor = dtor;
            tab[i].objsz = objsz;
            return &tab[i];
        }
    }
    return 0;
}

void remove_entry(uint64 kc) {
    for(int i=0;i<MAX_UCACHES;i++){
        if(tab[i].used && tab[i].kc == kc){
            tab[i].used = 0;
            tab[i].kc = 0;
            tab[i].ctor = 0;
            tab[i].dtor = 0;
            tab[i].objsz = 0;
            return;
        }
    }
}

void ucache_dump_one(uint64 kc) {
    struct ucent *e = find(kc);
    if(!e){
        printf("[ucache] kc=%p not found\n", (void*)kc);
        return;
    }
    printf("[ucache] kc=%p objsz=%d ctor=%p dtor=%p\n",
           (void*)e->kc, e->objsz, (void*)e->ctor, (void*)e->dtor);
}

void ucache_dump_all(void) {
    printf("[ucache] dump all\n");
    for(int i=0;i<MAX_UCACHES;i++){
        if(tab[i].used){
            printf("  slot=%d kc=%p objsz=%d ctor=%p dtor=%p\n",
                   i, (void*)tab[i].kc, tab[i].objsz, (void*)tab[i].ctor, (void*)tab[i].dtor);
        }
    }
}