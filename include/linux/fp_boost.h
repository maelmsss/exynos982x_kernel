#ifndef _LINUX_FP_BOOST_H
#define _LINUX_FP_BOOST_H

#ifdef CONFIG_FINGERPRINT_BOOST
void fp_boost_kick(void);
void fp_boost_relax(void);
#else
static inline void fp_boost_kick(void) { }
static inline void fp_boost_relax(void) { }
#endif

#endif
