#ifndef DRUN_H
#define DRUN_H

#include "desktop_vec.h"

struct desktop_vec drun_generate(void);
struct desktop_vec drun_generate_cached(void);
void drun_launch(const char *filename);

#endif /* DRUN_H */
