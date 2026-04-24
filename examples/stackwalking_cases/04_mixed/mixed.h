#ifndef MIXED_H
#define MIXED_H

/* cfi_part.c — compiled with full CFI (.eh_frame + debug info) */
void cfi_relay_outer(void);
void cfi_relay_inner(void);

/* fp_part.c — compiled with frame pointers, no .eh_frame */
void fp_relay_outer(void);
void fp_relay_inner(void);

/* scan_part.c — compiled without frame pointers and without .eh_frame */
void scan_relay(void);
void scan_crash(void);

#endif /* MIXED_H */
