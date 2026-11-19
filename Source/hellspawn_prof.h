#pragma once
/* Mediciones de tiempo del DSi (NDS-RAPIDO5). En otras plataformas no hacen nada. */
#ifdef __NDS__
extern "C" unsigned nds_usec(void);
extern "C" void nds_prof_add(int which, unsigned us);
#else
static inline unsigned nds_usec(void) { return 0; }
static inline void nds_prof_add(int, unsigned) {}
#endif
